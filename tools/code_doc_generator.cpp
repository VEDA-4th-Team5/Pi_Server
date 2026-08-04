#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct FunctionInfo {
    std::string name;
    std::string signature;
    std::string description;
};

struct FileInfo {
    std::string path;
    std::string description;
    std::vector<FunctionInfo> functions;
};

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read: " + path.string());
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

void writeText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write: " + temporary.string());
        }
        output << text;
        if (!output) {
            throw std::runtime_error("write failed: " + temporary.string());
        }
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
    }
    if (error) throw std::runtime_error("rename failed: " + error.message());
}

std::string replaceAll(std::string value, const std::string& from,
                       const std::string& to) {
    std::size_t position{};
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
    return value;
}

std::string decodeXml(std::string value) {
    value = replaceAll(std::move(value), "&lt;", "<");
    value = replaceAll(std::move(value), "&gt;", ">");
    value = replaceAll(std::move(value), "&quot;", "\"");
    value = replaceAll(std::move(value), "&apos;", "'");
    value = replaceAll(std::move(value), "&amp;", "&");
    return value;
}

std::string collapseWhitespace(const std::string& value) {
    std::string output;
    bool whitespace{};
    for (const unsigned char character : value) {
        if (std::isspace(character)) {
            whitespace = !output.empty();
        } else {
            if (whitespace) output.push_back(' ');
            output.push_back(static_cast<char>(character));
            whitespace = false;
        }
    }
    while (!output.empty() && output.back() == ' ') output.pop_back();
    return output;
}

std::string stripXml(const std::string& xml) {
    std::string output;
    bool in_tag{};
    for (const char character : xml) {
        if (character == '<') {
            in_tag = true;
        } else if (character == '>') {
            in_tag = false;
            output.push_back(' ');
        } else if (!in_tag) {
            output.push_back(character);
        }
    }
    return collapseWhitespace(decodeXml(std::move(output)));
}

std::string tagValue(const std::string& xml, const std::string& tag,
                     const std::size_t start = 0) {
    const std::string opening = "<" + tag;
    const auto open = xml.find(opening, start);
    if (open == std::string::npos) return {};
    const auto content_start = xml.find('>', open);
    if (content_start == std::string::npos) return {};
    const std::string closing = "</" + tag + ">";
    const auto close = xml.find(closing, content_start + 1);
    if (close == std::string::npos) return {};
    return stripXml(xml.substr(content_start + 1, close - content_start - 1));
}

std::string attributeValue(const std::string& opening,
                           const std::string& attribute) {
    const std::string key = attribute + "=\"";
    const auto begin = opening.find(key);
    if (begin == std::string::npos) return {};
    const auto value_begin = begin + key.size();
    const auto end = opening.find('"', value_begin);
    if (end == std::string::npos) return {};
    return opening.substr(value_begin, end - value_begin);
}

std::string markdownCode(std::string value) {
    value = replaceAll(std::move(value), "`", "'");
    return "`" + value + "`";
}

std::string fallbackFileDescription(const std::string& path) {
    if (path.starts_with("include/"))
        return "공개 인터페이스, 타입 또는 클래스 선언을 정의한다.";
    if (path.starts_with("src/"))
        return "Raspberry Pi 서버의 런타임 구현을 담당한다.";
    if (path.starts_with("tests/"))
        return "자동 테스트와 회귀 검증 시나리오를 구현한다.";
    if (path.starts_with("driver/"))
        return "Linux 커널 드라이버 또는 사용자 공간 ABI를 구현한다.";
    if (path.starts_with("tools/"))
        return "개발·운영·문서화를 지원하는 독립 도구다.";
    return "프로젝트 C/C++ 구성 파일이다.";
}

std::string normalizePath(const fs::path& project_root, std::string path) {
    path = replaceAll(std::move(path), "\\", "/");
    const std::string root = project_root.lexically_normal().generic_string();
    if (path.starts_with(root + "/")) path.erase(0, root.size() + 1);
    while (path.starts_with("../")) path.erase(0, 3);
    return path;
}

std::vector<FileInfo> parseDoxygen(const fs::path& project_root,
                                   const fs::path& xml_directory) {
    const std::string index = readText(xml_directory / "index.xml");
    std::map<std::string, FileInfo> files_by_path;
    std::size_t position{};
    while ((position = index.find("<compound ", position)) !=
           std::string::npos) {
        const auto opening_end = index.find('>', position);
        const auto compound_end = index.find("</compound>", opening_end);
        if (opening_end == std::string::npos ||
            compound_end == std::string::npos) break;
        const std::string opening =
            index.substr(position, opening_end - position + 1);
        position = compound_end + 11;
        if (attributeValue(opening, "kind") != "file") continue;
        const std::string refid = attributeValue(opening, "refid");
        if (refid.empty()) continue;

        const fs::path compound_path = xml_directory / (refid + ".xml");
        if (!fs::is_regular_file(compound_path)) continue;
        const std::string compound = readText(compound_path);
        FileInfo file;
        const auto location_position = compound.rfind("<location ");
        if (location_position != std::string::npos) {
            const auto location_end = compound.find('>', location_position);
            file.path = normalizePath(project_root, attributeValue(
                compound.substr(location_position,
                                location_end - location_position + 1),
                "file"));
        }
        if (file.path.empty()) {
            file.path = normalizePath(project_root,
                                      tagValue(compound, "compoundname"));
        }
        if (file.path.empty()) continue;
        const auto brief_begin = compound.find("<briefdescription>");
        const auto brief_end = compound.find("</briefdescription>", brief_begin);
        if (brief_begin != std::string::npos && brief_end != std::string::npos) {
            file.description = stripXml(compound.substr(
                brief_begin, brief_end - brief_begin + 19));
        }
        if (file.description.empty())
            file.description = fallbackFileDescription(file.path);

        files_by_path[file.path] = std::move(file);
    }

    // Class/namespace 함수는 file compound가 아니라 별도 compound XML에
    // 기록되므로 모든 XML의 memberdef를 location 기준으로 원래 파일에 합친다.
    for (const auto& entry : fs::directory_iterator(xml_directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".xml" ||
            entry.path().filename() == "index.xml") {
            continue;
        }
        const std::string compound = readText(entry.path());
        std::size_t member_position{};
        while ((member_position = compound.find(
                    "<memberdef kind=\"function\"", member_position)) !=
               std::string::npos) {
            const auto member_end = compound.find("</memberdef>", member_position);
            if (member_end == std::string::npos) break;
            const std::string member = compound.substr(
                member_position, member_end - member_position + 12);
            member_position = member_end + 12;
            FunctionInfo function;
            function.name = tagValue(member, "name");
            const std::string definition = tagValue(member, "definition");
            const std::string arguments = tagValue(member, "argsstring");
            function.signature = collapseWhitespace(definition + arguments);
            const auto member_brief_begin = member.find("<briefdescription>");
            const auto member_brief_end = member.find(
                "</briefdescription>", member_brief_begin);
            if (member_brief_begin != std::string::npos &&
                member_brief_end != std::string::npos) {
                function.description = stripXml(member.substr(
                    member_brief_begin,
                    member_brief_end - member_brief_begin + 19));
            }
            if (function.description.empty()) {
                function.description =
                    "Doxygen 설명이 없어 선언과 호출부를 함께 확인해야 한다.";
            }
            if (function.name.empty()) continue;

            const auto location_position = member.find("<location ");
            if (location_position == std::string::npos) continue;
            const auto location_end = member.find('>', location_position);
            if (location_end == std::string::npos) continue;
            const std::string location = member.substr(
                location_position, location_end - location_position + 1);
            std::vector<std::string> targets;
            for (const auto* attribute : {"file", "bodyfile"}) {
                std::string target = normalizePath(
                    project_root, attributeValue(location, attribute));
                if (!target.empty() &&
                    std::find(targets.begin(), targets.end(), target) ==
                        targets.end()) {
                    targets.push_back(std::move(target));
                }
            }
            for (const auto& target : targets) {
                auto found = files_by_path.find(target);
                if (found == files_by_path.end()) continue;
                const bool duplicate = std::any_of(
                    found->second.functions.begin(),
                    found->second.functions.end(),
                    [&function](const FunctionInfo& existing) {
                        return existing.signature == function.signature;
                    });
                if (!duplicate) found->second.functions.push_back(function);
            }
        }
    }

    std::vector<FileInfo> files;
    files.reserve(files_by_path.size());
    for (auto& [path, file] : files_by_path) {
        std::sort(file.functions.begin(), file.functions.end(),
                  [](const auto& left, const auto& right) {
                      return left.signature < right.signature;
                  });
        files.push_back(std::move(file));
    }
    std::sort(files.begin(), files.end(),
              [](const auto& left, const auto& right) {
                  return left.path < right.path;
              });
    return files;
}

std::string buildMarkdown(const std::string& overview,
                          const std::vector<FileInfo>& files) {
    std::ostringstream output;
    output << "# Pi Server 코드 이해 가이드\n\n"
           << "> 이 파일은 자동 생성됩니다. 직접 수정하지 말고 C/C++ 소스의 "
              "Doxygen 주석 또는 `docs/architecture/README.md`를 수정한 뒤 "
              "문서 빌드를 다시 실행하십시오.\n\n"
           << "- 생성 기준: 현재 작업 트리\n"
           << "- 분석 파일 수: " << files.size() << "\n"
           << "- 생성 명령: `cmake --build cmake-build --target docs`\n\n"
           << overview << "\n\n"
           << "# 자동 생성 소스 파일·함수 참조\n\n"
           << "이 절은 Doxygen XML에서 추출한다. 함수 설명이 비어 있다면 해당 함수에 "
              "`/** @brief ... */` 주석을 추가한 후 문서를 다시 생성한다.\n\n";

    std::string current_directory;
    for (const auto& file : files) {
        const std::string directory = fs::path(file.path).parent_path().string();
        if (directory != current_directory) {
            current_directory = directory;
            output << "## " << (directory.empty() ? "project root" : directory)
                   << "\n\n";
        }
        output << "### " << file.path << "\n\n"
               << file.description << "\n\n";
        if (file.functions.empty()) {
            output << "- 함수 없음: 타입·상수·구조체 선언 또는 데이터 전용 파일\n\n";
            continue;
        }
        for (const auto& function : file.functions) {
            output << "- " << markdownCode(
                function.signature.empty() ? function.name : function.signature)
                   << " — " << function.description << "\n";
        }
        output << '\n';
    }
    std::string markdown = output.str();
    while (!markdown.empty() && markdown.back() == '\n') markdown.pop_back();
    markdown.push_back('\n');
    return markdown;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 5) {
            std::cerr << "usage: code-doc-generator <project-root> <xml-dir> "
                         "<overview-md> <output-md>\n";
            return 2;
        }
        const fs::path project_root = fs::absolute(argv[1]).lexically_normal();
        const fs::path xml_directory = fs::absolute(argv[2]).lexically_normal();
        const fs::path overview_path = fs::absolute(argv[3]).lexically_normal();
        const fs::path output_path = fs::absolute(argv[4]).lexically_normal();
        const auto files = parseDoxygen(project_root, xml_directory);
        if (files.empty()) throw std::runtime_error("no C/C++ files found in XML");
        writeText(output_path,
                  buildMarkdown(readText(overview_path), files));
        std::cout << "generated Markdown: " << output_path << " ("
                  << files.size() << " files)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "code document generation failed: " << error.what() << '\n';
        return 1;
    }
}
