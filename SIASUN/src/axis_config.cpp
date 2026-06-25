#include "axis_config.h"

#include <cstdio>
#include <fstream>
#include <sys/stat.h>

#include "app_config.h"
#include "tinyxml2.h"

namespace sinsun {
namespace {

/*
 * 判断路径是否为可访问目录。
 *
 * path：待检查路径。
 */
bool is_directory(const std::string &path) {
    /* st：stat() 填充的文件系统状态。 */
    struct stat st {};

    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * 拼接目录和文件名。
 *
 * directory：目录路径。
 * filename：文件名。
 */
std::string join_path(const std::string &directory, const std::string &filename) {
    if (directory.empty() || directory.back() == '/') {
        return directory + filename;
    }

    return directory + "/" + filename;
}

/*
 * 解析单个 ServoParameters XML 节点。
 *
 * element：tinyxml2 定位到的 ServoParameters 节点。
 * parameter：输出参数。
 */
bool parse_servo_parameter(const tinyxml2::XMLElement *element,
                           ServoParameter &parameter) {
    /* id_node：参数 ID 节点。 */
    const tinyxml2::XMLElement *id_node = element->FirstChildElement("Id");

    /* name_node：参数名称节点。 */
    const tinyxml2::XMLElement *name_node = element->FirstChildElement("Name");

    /* value_node：参数值节点。 */
    const tinyxml2::XMLElement *value_node = element->FirstChildElement("Value");

    /* qfmt_node：参数定点缩放位数节点。 */
    const tinyxml2::XMLElement *qfmt_node = element->FirstChildElement("qFmt");

    if (!id_node || !value_node || !qfmt_node) {
        return false;
    }

    parameter.id = id_node->IntText(-1);
    if (parameter.id < 0) {
        return false;
    }

    if (name_node && name_node->GetText()) {
        parameter.name = name_node->GetText();
    }

    parameter.value = value_node->DoubleText(0.0);
    parameter.qfmt = qfmt_node->IntText(0);
    return true;
}

/*
 * 解析单个 Axis*.xml 文件。
 *
 * path：Axis XML 文件路径。
 * parameters：输出该轴参数列表。
 */
int load_axis_file(const std::string &path, AxisParameters &parameters) {
    std::printf("[XML] loading axis parameter file: %s\n", path.c_str());

    /* doc：tinyxml2 XML 文档对象。 */
    tinyxml2::XMLDocument doc;

    /* load_result：tinyxml2 文件加载结果。 */
    const tinyxml2::XMLError load_result = doc.LoadFile(path.c_str());

    if (load_result != tinyxml2::XML_SUCCESS) {
        std::fprintf(stderr, "failed to load %s: %s\n", path.c_str(),
                     doc.ErrorStr());
        return -1;
    }

    /* root：Axis XML 根节点 dataentry。 */
    const tinyxml2::XMLElement *root = doc.FirstChildElement("dataentry");

    if (!root) {
        std::fprintf(stderr, "missing dataentry root in %s\n", path.c_str());
        return -1;
    }

    /* total_count：文件中遍历到的 ServoParameters 节点总数。 */
    size_t total_count = 0;

    /* skipped_count：因缺字段或 id<0 被跳过的 ServoParameters 节点数。 */
    size_t skipped_count = 0;

    /* element：当前遍历到的 ServoParameters 节点。 */
    for (const tinyxml2::XMLElement *element =
             root->FirstChildElement("ServoParameters");
         element;
         element = element->NextSiblingElement("ServoParameters")) {
        ++total_count;

        /* parameter：当前 ServoParameters 节点解析出来的参数。 */
        ServoParameter parameter;

        if (parse_servo_parameter(element, parameter)) {
            parameters.push_back(parameter);
            if (kLogAxisXmlParameterDetails) {
                std::printf("[XML]   valid id=%d name=%s value=%.10g qFmt=%d\n",
                            parameter.id, parameter.name.c_str(),
                            parameter.value, parameter.qfmt);
            }
        } else {
            ++skipped_count;
            if (kLogAxisXmlParameterDetails) {
                std::printf("[XML]   skipped ServoParameters node #%zu\n",
                            total_count);
            }
        }
    }

    if (parameters.empty()) {
        std::fprintf(stderr, "no valid ServoParameters in %s\n", path.c_str());
        return -1;
    }

    std::printf("[XML] loaded %zu valid parameters from %s "
                "(nodes=%zu skipped=%zu)\n",
                parameters.size(), path.c_str(), total_count, skipped_count);

    return 0;
}

}  // namespace

int resolve_axis_config_directory(const std::string &requested_directory,
                                  std::string &selected_directory) {
    /* candidates：按优先级尝试的 Axis*.xml 目录。 */
    std::vector<std::string> candidates;

    if (!requested_directory.empty()) {
        candidates.push_back(requested_directory);
    } else {
        candidates.push_back("SIASUN/gcr10_1300");
    }

    for (const auto &candidate : candidates) {
        /* axis1_path：用于确认目录有效性的 Axis1.xml 路径。 */
        const std::string axis1_path = join_path(candidate, "Axis1.xml");

        std::printf("[XML] checking Axis*.xml directory: %s\n",
                    candidate.c_str());

        if (is_directory(candidate)) {
            /* input：尝试打开 Axis1.xml，确认目录确实包含轴参数文件。 */
            std::ifstream input(axis1_path);

            if (input.good()) {
                std::printf("[XML] selected Axis*.xml directory: %s\n",
                            candidate.c_str());
                selected_directory = candidate;
                return 0;
            }
        } else {
            std::printf("[XML]   not a directory: %s\n", candidate.c_str());
        }
    }

    std::fprintf(stderr, "failed to resolve Axis*.xml directory\n");
    return -1;
}

int load_axis_parameter_set(const std::string &directory,
                            AxisParameterSet &parameters) {
    for (size_t axis = 0; axis < kServoCount; ++axis) {
        /* filename：当前轴的 Axis*.xml 文件名。 */
        const std::string filename = "Axis" + std::to_string(axis + 1) + ".xml";

        /* path：当前轴的 Axis*.xml 完整路径。 */
        const std::string path = join_path(directory, filename);

        if (load_axis_file(path, parameters[axis])) {
            return -1;
        }
    }

    std::printf("[XML] all Axis*.xml files loaded successfully from %s\n",
                directory.c_str());

    return 0;
}

}  // namespace sinsun
