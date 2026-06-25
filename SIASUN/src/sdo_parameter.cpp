#include "sdo_parameter.h"

#include <cmath>
#include <cstdio>

namespace sinsun {
namespace {

/* SDO 写入三步协议中的 mailbox word 类型，用于日志打印。 */
enum class MailboxWordKind {
    Header,
    Value,
    Id,
};

/*
 * 将 mailbox word 类型转换为日志文本。
 *
 * kind：mailbox word 类型。
 */
const char *mailbox_word_kind_name(MailboxWordKind kind) {
    switch (kind) {
    case MailboxWordKind::Header:
        return "header";
    case MailboxWordKind::Value:
        return "value";
    case MailboxWordKind::Id:
        return "id";
    }

    return "unknown";
}

/*
 * 将 32-bit SDO 参数写到指定伺服的 0x2020:00。
 *
 * master：IgH master 句柄。
 * slave_position：IgH 0-based 从站位置。
 * word：要写入的 32-bit 参数字。
 * kind：当前写入的是 header/value/id 三步中的哪一步。
 * parameter：当前下发的参数，仅用于日志。
 */
int download_mailbox_word(ec_master_t *master,
                          uint16_t slave_position,
                          uint32_t word,
                          MailboxWordKind kind,
                          const ServoParameter &parameter) {
    /* data：SDO download 的小端 4 字节 payload。 */
    uint8_t data[4] {};

    /* abort_code：IgH 返回的 SDO abort code，非 0 时用于现场排查。 */
    uint32_t abort_code = 0;

    EC_WRITE_U32(data, word);

    if (kLogSdoDownloadDetails) {
        std::printf("[SDO] servo=%u param id=%d name=%s step=%s "
                    "download 0x2020:00 word=0x%08X\n",
                    static_cast<unsigned int>(slave_position + 1),
                    parameter.id, parameter.name.c_str(),
                    mailbox_word_kind_name(kind), word);
    }

    const int ret = ecrt_master_sdo_download(master, slave_position, 0x2020, 0x00,
                                             data, sizeof(data), &abort_code);
    if (ret) {
        std::fprintf(stderr,
                     "SDO download failed: position=%u word=0x%08X ret=%d "
                     "abort=0x%08X\n",
                     slave_position, word, ret, abort_code);
    } else if (kLogSdoDownloadDetails) {
        std::printf("[SDO] servo=%u param id=%d step=%s ok\n",
                    static_cast<unsigned int>(slave_position + 1),
                    parameter.id, mailbox_word_kind_name(kind));
    }

    return ret;
}

/*
 * 将参数 value/qFmt 编码成示例协议使用的 32-bit 数据字。
 *
 * value：Axis*.xml 中的参数值。
 * qfmt：Axis*.xml 中的定点缩放位数。
 */
uint32_t encode_parameter_value(double value, int qfmt) {
    /* sign：负数时置 bit16，和 siasun_res 示例保持一致。 */
    const uint32_t sign = value < 0.0 ? 0x10000U : 0U;

    /* scaled：abs(value) * 2^qfmt 后的 16-bit 幅值。 */
    const auto scaled = static_cast<uint16_t>(
        std::fabs(value) * std::pow(2.0, static_cast<double>(qfmt)));

    return sign + scaled;
}

}  // namespace

int write_servo_parameter(ec_master_t *master,
                          uint16_t slave_position,
                          const ServoParameter &parameter) {
    if (kLogSdoDownloadDetails) {
        std::printf("[SDO] servo=%u begin param id=%d name=%s value=%.10g "
                    "qFmt=%d\n",
                    static_cast<unsigned int>(slave_position + 1),
                    parameter.id, parameter.name.c_str(), parameter.value,
                    parameter.qfmt);
    }

    /* header_word：写参数 mailbox 的头字，来自 siasun_res 示例 step1。 */
    const uint32_t header_word = (0x02U << 24) | (0x01U << 16) |
                                 (0x63U << 8) | 0x01U;

    if (download_mailbox_word(master, slave_position, header_word,
                              MailboxWordKind::Header, parameter)) {
        return -1;
    }

    /* value_word：参数值数据字，按 value/qFmt 编码。 */
    const uint32_t value_word =
        encode_parameter_value(parameter.value, parameter.qfmt);

    if (download_mailbox_word(master, slave_position, value_word,
                              MailboxWordKind::Value, parameter)) {
        return -1;
    }

    /* id_word：参数 ID 数据字，来自 siasun_res 示例 step3。 */
    const uint32_t id_word = 0x10000U + static_cast<uint32_t>(parameter.id);

    if (download_mailbox_word(master, slave_position, id_word,
                              MailboxWordKind::Id, parameter)) {
        return -1;
    }

    if (kLogSdoDownloadDetails) {
        std::printf("[SDO] servo=%u finish param id=%d name=%s\n",
                    static_cast<unsigned int>(slave_position + 1),
                    parameter.id, parameter.name.c_str());
    }

    return 0;
}

int write_axis_parameters(ec_master_t *master,
                          const AxisParameterSet &parameters) {
    for (size_t axis = 0; axis < kServoCount; ++axis) {
        /* slave_position：IgH 0-based 伺服位置，axis 0-5 对应 id 1-6。 */
        const uint16_t slave_position = static_cast<uint16_t>(axis);

        std::printf("[SDO] servo %zu writing %zu XML parameters\n",
                    axis + 1, parameters[axis].size());

        /* written_count：当前轴已成功下发的 XML 参数数量。 */
        size_t written_count = 0;

        for (const auto &parameter : parameters[axis]) {
            if (write_servo_parameter(master, slave_position, parameter)) {
                std::fprintf(stderr,
                             "failed to write servo %zu parameter id=%d "
                             "name=%s value=%f qfmt=%d\n",
                             axis + 1, parameter.id, parameter.name.c_str(),
                             parameter.value, parameter.qfmt);
                return -1;
            }
            ++written_count;
        }

        std::printf("[SDO] servo %zu XML parameters written: %zu/%zu\n",
                    axis + 1, written_count, parameters[axis].size());

        /* apply_parameter：id=2,value=1,qFmt=0，用于生效已下载配置参数。 */
        const ServoParameter apply_parameter{2, "tRequestParaFlag", 1.0, 0};

        std::printf("[SDO] servo %zu applying downloaded parameters\n", axis + 1);
        if (write_servo_parameter(master, slave_position, apply_parameter)) {
            std::fprintf(stderr, "failed to apply parameters on servo %zu\n",
                         axis + 1);
            return -1;
        }

        std::printf("[SDO] servo %zu SDO parameter setup complete\n", axis + 1);
    }

    std::printf("[SDO] all servo SDO parameters written successfully\n");
    return 0;
}

}  // namespace sinsun
