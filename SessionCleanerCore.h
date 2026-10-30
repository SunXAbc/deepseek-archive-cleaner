#pragma once

#include <afxwin.h>
#include <tlhelp32.h>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

// 文件系统路径始终使用 Windows 宽字符，避免中文目录被本地代码页截断。
namespace fs = std::filesystem;

// 仅在显式传入 -root 时覆盖当前用户的数据目录，不写入配置或日志。
static CString g_rootOverride;

/** @return 当前用户的数据根目录；-root 只影响本进程，不修改系统环境。 */
static CString GetDshRoot()
{
    if (!g_rootOverride.IsEmpty()) return g_rootOverride;
    // 用户目录来自 Windows 环境，仅在当前进程内使用。
    wchar_t userDirectory[32768]{};
    if (!GetEnvironmentVariableW(L"USERPROFILE", userDirectory, _countof(userDirectory)))
        throw CString(L"无法确定当前用户目录。");
    return CString(userDirectory) + L"\\.dsh";
}

/** 只保存需要读取的 JSON 结构及源文本范围，不重排无关数据。 */
struct Json
{
    /** 节点类别；布尔和数字只验证语法，业务代码不解释其值。 */
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
    // 本节点的 JSON 类别。
    Type type = NUL;
    // 本节点在原始 UTF-8 文本中的起始字节偏移。
    size_t begin = 0;
    // 本节点之后的第一个字节偏移，区间为 [begin, end)。
    size_t end = 0;
    // 已解码的字符串内容，生命周期与节点一致。
    std::string str;
    // 数组元素保持原有顺序。
    std::vector<Json> arr;
    // 对象成员由当前节点持有；重复键在解析时拒绝。
    std::map<std::string, Json> obj;

    /** @param key 对象成员名。@return 借用的子节点地址；不存在或类型不符时返回 nullptr。 */
    const Json* get(const char* key) const
    {
        // 查找结果仅借用当前节点的成员，不复制会话内容。
        const auto found = obj.find(key);
        return type == OBJ && found != obj.end() ? &found->second : nullptr;
    }

    /** @param key 字符串成员名。@return 已解码的字符串副本，缺失时为空。 */
    std::string s(const char* key) const
    {
        // 子节点的生命周期由当前对象持有。
        const Json* child = get(key);
        return child && child->type == STR ? child->str : std::string();
    }
};

/** @param output 接收 UTF-8 字节的字符串。@param codePoint 已校验的 Unicode 码位；无返回值。 */
static void AppendUtf8(std::string& output, unsigned codePoint)
{
    if (codePoint < 0x80) output += (char)codePoint;
    else if (codePoint < 0x800)
    {
        output += (char)(0xC0 | (codePoint >> 6));
        output += (char)(0x80 | (codePoint & 0x3F));
    }
    else if (codePoint < 0x10000)
    {
        output += (char)(0xE0 | (codePoint >> 12));
        output += (char)(0x80 | ((codePoint >> 6) & 0x3F));
        output += (char)(0x80 | (codePoint & 0x3F));
    }
    else
    {
        output += (char)(0xF0 | (codePoint >> 18));
        output += (char)(0x80 | ((codePoint >> 12) & 0x3F));
        output += (char)(0x80 | ((codePoint >> 6) & 0x3F));
        output += (char)(0x80 | (codePoint & 0x3F));
    }
}

/** 严格解析完整 JSON；限制嵌套深度，拒绝重复键、残缺数据和不合法转义。 */
class JsonParser
{
public:
    /** @param text 借用的 UTF-8 源文本，须在解析期间保持有效。 */
    explicit JsonParser(const std::string& text) : m_text(text) {}

    /** @param output 接收解析树。@return 仅当整个文本合法时返回 true。 */
    bool parse(Json& output)
    {
        // BOM 只允许在文件开头出现，偏移仍对应原始字节，以便精确替换。
        m_position = m_text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
        m_depth = 0;
        output = Json();
        if (!value(output)) return false;
        skip();
        return m_position == m_text.size();
    }

private:
    // 源文本不由解析器持有，业务调用在树使用期间保留其副本。
    const std::string& m_text;
    // 下一个待解析字节在源文本中的位置。
    size_t m_position = 0;
    // 当前递归层数，超过 128 层即拒绝。
    unsigned m_depth = 0;

    /** 跳过 JSON 允许的四种 ASCII 空白，无参数和返回值。 */
    void skip()
    {
        while (m_position < m_text.size() &&
               (m_text[m_position] == ' ' || m_text[m_position] == '\t' ||
                m_text[m_position] == '\r' || m_text[m_position] == '\n')) ++m_position;
    }

    /** @param literal 待匹配的 JSON 字面量。@return 匹配成功时前移游标并返回 true。 */
    bool literal(const char* literal)
    {
        // ASCII 字面量长度，单位为字节。
        const size_t length = strlen(literal);
        if (m_text.compare(m_position, length, literal) != 0) return false;
        m_position += length;
        return true;
    }

    /** @param output 接收单个节点。@return 当前节点是否完整合法。 */
    bool value(Json& output)
    {
        skip();
        if (m_position >= m_text.size() || m_depth >= 128) return false;
        ++m_depth;
        output.begin = m_position;
        // 每个节点完成后统一记录结束位置并恢复递归深度。
        bool valid = false;
        switch (m_text[m_position])
        {
        case '{': valid = object(output); break;
        case '[': valid = array(output); break;
        case '"': output.type = Json::STR; valid = string(output.str); break;
        case 't': output.type = Json::BOOL; valid = literal("true"); break;
        case 'f': output.type = Json::BOOL; valid = literal("false"); break;
        case 'n': output.type = Json::NUL; valid = literal("null"); break;
        default: valid = number(output); break;
        }
        output.end = m_position;
        --m_depth;
        return valid;
    }

    /** @param output 接收对象成员。@return 对象是否合法；重复键和尾逗号均拒绝。 */
    bool object(Json& output)
    {
        output.type = Json::OBJ;
        ++m_position;
        skip();
        if (m_position < m_text.size() && m_text[m_position] == '}') { ++m_position; return true; }
        while (m_position < m_text.size())
        {
            skip();
            // 当前对象成员名，解码后用于重复键检查。
            std::string key;
            if (!string(key) || output.obj.count(key)) return false;
            skip();
            if (m_position >= m_text.size() || m_text[m_position++] != ':') return false;
            // 当前成员值，解析成功后转移给所属对象。
            Json child;
            if (!value(child)) return false;
            output.obj.emplace(key, std::move(child));
            skip();
            if (m_position >= m_text.size()) return false;
            if (m_text[m_position] == '}') { ++m_position; return true; }
            if (m_text[m_position++] != ',') return false;
        }
        return false;
    }

    /** @param output 接收有序数组元素。@return 数组是否完整且不含尾逗号。 */
    bool array(Json& output)
    {
        output.type = Json::ARR;
        ++m_position;
        skip();
        if (m_position < m_text.size() && m_text[m_position] == ']') { ++m_position; return true; }
        while (m_position < m_text.size())
        {
            // 单个数组元素，解析后由数组拥有。
            Json child;
            if (!value(child)) return false;
            output.arr.push_back(std::move(child));
            skip();
            if (m_position >= m_text.size()) return false;
            if (m_text[m_position] == ']') { ++m_position; return true; }
            if (m_text[m_position++] != ',') return false;
        }
        return false;
    }

    /** @param output 接收四位十六进制数。@return 是否读到恰好四个合法十六进制字符。 */
    bool hex4(unsigned& output)
    {
        output = 0;
        if (m_position + 4 > m_text.size()) return false;
        // digitIndex 表示当前四位转义中的字符序号。
        for (unsigned digitIndex = 0; digitIndex < 4; ++digitIndex)
        {
            // 当前十六进制 ASCII 字符。
            const char digit = m_text[m_position++];
            output <<= 4;
            if (digit >= '0' && digit <= '9') output |= digit - '0';
            else if (digit >= 'a' && digit <= 'f') output |= digit - 'a' + 10;
            else if (digit >= 'A' && digit <= 'F') output |= digit - 'A' + 10;
            else return false;
        }
        return true;
    }

    /** @param output 接收解码后的 UTF-8 字符串。@return 字符串与 Unicode 转义是否合法。 */
    bool string(std::string& output)
    {
        if (m_position >= m_text.size() || m_text[m_position++] != '"') return false;
        while (m_position < m_text.size())
        {
            // 当前源字节；控制字符必须经过 JSON 转义。
            const unsigned char character = (unsigned char)m_text[m_position++];
            if (character == '"') return true;
            if (character < 0x20) return false;
            if (character != '\\') { output += (char)character; continue; }
            if (m_position >= m_text.size()) return false;
            // 反斜杠后的转义类型。
            const char escape = m_text[m_position++];
            switch (escape)
            {
            case '"': output += '"'; break;
            case '\\': output += '\\'; break;
            case '/': output += '/'; break;
            case 'b': output += '\b'; break;
            case 'f': output += '\f'; break;
            case 'n': output += '\n'; break;
            case 'r': output += '\r'; break;
            case 't': output += '\t'; break;
            case 'u':
            {
                // 完整 Unicode 码位；代理对必须成对且顺序正确。
                unsigned codePoint = 0;
                if (!hex4(codePoint)) return false;
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
                {
                    if (m_text.compare(m_position, 2, "\\u") != 0) return false;
                    m_position += 2;
                    // UTF-16 低代理，范围必须为 DC00 至 DFFF。
                    unsigned low = 0;
                    if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + low - 0xDC00;
                }
                else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) return false;
                AppendUtf8(output, codePoint);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    /** @return 当前游标是否为数字字符，不改变解析位置。 */
    bool digit() const
    {
        return m_position < m_text.size() && m_text[m_position] >= '0' && m_text[m_position] <= '9';
    }

    /** @param output 接收数字类型与源范围。@return 数字是否符合 JSON 数字语法。 */
    bool number(Json& output)
    {
        output.type = Json::NUM;
        if (m_position < m_text.size() && m_text[m_position] == '-') ++m_position;
        if (!digit()) return false;
        if (m_text[m_position] == '0') ++m_position;
        else while (digit()) ++m_position;
        if (m_position < m_text.size() && m_text[m_position] == '.')
        {
            ++m_position;
            if (!digit()) return false;
            while (digit()) ++m_position;
        }
        if (m_position < m_text.size() && (m_text[m_position] == 'e' || m_text[m_position] == 'E'))
        {
            ++m_position;
            if (m_position < m_text.size() && (m_text[m_position] == '+' || m_text[m_position] == '-')) ++m_position;
            if (!digit()) return false;
            while (digit()) ++m_position;
        }
        return true;
    }
};

/** 单个归档会话的展示快照；执行删除时重新定位路径，不信任旧快照。 */
struct SessionItem
{
    // 索引中的会话 ID，只允许安全的 ASCII 标识符。
    CString id;
    // 缓存中的会话标题，仅在本地界面显示。
    CString title;
    // 会话工作目录，仅用于展示，不作为删除目标。
    CString cwd;
    // sessions 下的工作区目录名，缺少标题时用于本地兜底。
    CString workspace;
    // 扫描时的目录绝对路径；删除时不直接使用此字段。
    CString path;
    // 扫描时的文件总长度，单位为字节。
    ULONGLONG bytes = 0;
    // 目录最后修改时间，使用 Windows FILETIME 单位。
    FILETIME mtime{};
};

/** @param text 已校验的 UTF-8 文本。@return Windows 宽字符字符串副本。 */
static CStringW FromUtf8(const std::string& text)
{
    if (text.empty()) return CStringW();
    // 解码后的 UTF-16 码元数，不含结尾零字符。
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (!length) throw CString(L"文件包含不合法的 UTF-8 数据。");
    // 本地字符串持有解码结果。
    CStringW output;
    // 借用 CString 内部缓冲区，仅在 ReleaseBuffer 前有效。
    wchar_t* buffer = output.GetBuffer(length);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), buffer, length);
    output.ReleaseBuffer(length);
    return output;
}

/** @param text Windows 宽字符文本。@return UTF-8 字节副本。 */
static std::string ToUtf8(const CString& text)
{
    if (text.IsEmpty()) return std::string();
    // UTF-8 输出长度，单位为字节。
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, text.GetLength(), nullptr, 0, nullptr, nullptr);
    if (!length) throw CString(L"会话信息包含不合法的 Unicode 数据。");
    // 标准字符串持有编码后的完整数据。
    std::string output(length, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, text.GetLength(), output.data(), length, nullptr, nullptr);
    return output;
}

/** @param path 本地绝对路径。@return 所有已存在的路径分量均不是重解析点且可检查时返回 true。 */
static bool IsPlainLocalPath(const fs::path& path)
{
    if (!path.is_absolute() || path.root_name().wstring().size() != 2 ||
        GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE) return false;
    // 从盘符开始逐级检查，不能只检查最后一层而遗漏父目录联接。
    fs::path current = path.root_path();
    // part 表示当前本地路径的一层名称，不接受上级跳转和备用数据流。
    for (const fs::path& part : path.relative_path())
    {
        if (part == L".." || part.wstring().find(L':') != std::wstring::npos) return false;
        current /= part;
        // Windows 属性用于同时识别符号链接、目录联接和其他重解析点。
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            // 尚不存在的尾部路径可在安全的普通父目录中创建。
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    }
    return true;
}

/** @return 标准化后的本地数据根目录。@throws CString 路径不可安全检查时拒绝操作。 */
static fs::path DataRoot()
{
    // 标准库错误使用显式输出参数，避免未捕获的文件系统异常。
    std::error_code error;
    // 根目录标准化后仍须通过逐级的 Windows 属性检查。
    const fs::path root = fs::absolute(fs::path(GetDshRoot().GetString()), error).lexically_normal();
    if (error || !IsPlainLocalPath(root)) throw CString(L"数据目录必须是普通本地目录，不能经过链接、联接或网络盘。");
    return root;
}

/** @param path 待读取的普通本地文件。@return 原始 UTF-8 字节，保留 BOM；读取失败时抛出 CString。 */
static std::string ReadAllUtf8(const fs::path& path)
{
    if (!IsPlainLocalPath(path)) throw CString(L"拒绝读取链接或无法安全检查的文件。");
    // Windows 文件句柄在每个成功或失败分支中均由本函数关闭。
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw CString(L"无法读取数据文件，请检查文件是否存在、被占用或无访问权限。");
    // 文件长度限制为 64 MiB，避免错误文件耗尽进程内存。
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 || length.QuadPart > 64ll * 1024 * 1024)
    {
        CloseHandle(file);
        throw CString(L"数据文件过大或无法读取其长度（最大 64 MiB）。");
    }
    // 读取结果由调用者持有，不缓存到日志或其他目录。
    std::string text((size_t)length.QuadPart, '\0');
    // 实际读取长度，以字节为单位，必须等于已取得的文件长度。
    DWORD read = 0;
    // 空文件仍视为成功读取，后续 JSON 解析会将其报告为格式错误。
    const bool success = text.empty() || ReadFile(file, text.data(), (DWORD)text.size(), &read, nullptr);
    CloseHandle(file);
    if (!success || read != text.size()) throw CString(L"数据文件未能完整读取，已停止操作。");
    if (!text.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0))
        throw CString(L"数据文件不是合法 UTF-8，已停止操作。");
    return text;
}

/** @param id 索引中的会话 ID。@return 是否为安全的 session- 标识符；路径分隔符、点和冒号均拒绝。 */
static bool IsSessionId(const std::string& id)
{
    if (id.size() <= 8 || id.size() > 128 || id.compare(0, 8, "session-") != 0) return false;
    // character 是标识符后缀的 ASCII 字节；不接受本地化的字母分类。
    for (const unsigned char character : id.substr(8))
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '-')) return false;
    return true;
}

/** @param document 已解析的 workspace 根对象。@return global.archivedSessionIds 数组的借用引用。 */
static const Json& ArchivedArray(const Json& document)
{
    // 只读取已确认的数据层级，不搜索正文或其他对象中的同名字段。
    const Json* global = document.get("global");
    // 归档数组必须存在且类型正确，否则不能把错误格式当作空列表处理。
    const Json* archived = global ? global->get("archivedSessionIds") : nullptr;
    if (!archived || archived->type != Json::ARR) throw CString(L"无法识别归档索引：需要 global.archivedSessionIds 数组。");
    return *archived;
}

/** @param text 完整 workspace 文本。@return 合法且去重的归档 ID，保留首次出现顺序。 */
static std::vector<std::string> ParseArchivedIds(const std::string& text)
{
    // 解析树只在当前调用中持有，避免跨文件混用源偏移。
    Json document;
    // 解析器借用 text，不拥有其内存。
    JsonParser parser(text);
    if (!parser.parse(document)) throw CString(L"归档索引 JSON 不完整或格式错误，已停止操作。");
    // 归档数组属于 document，引用仅在本次调用内使用。
    const Json& archived = ArchivedArray(document);
    // 返回给扫描或删除流程的有序 ID。
    std::vector<std::string> result;
    // 重复 ID 只扫描一次，但重写时仍会完整移除成功删除的重复条目。
    std::set<std::string> seen;
    // entry 表示当前数组元素，必须是合法字符串标识符。
    for (const Json& entry : archived.arr)
    {
        if (entry.type != Json::STR || !IsSessionId(entry.str)) throw CString(L"归档索引含有不安全或无法识别的会话 ID，已停止操作。");
        if (seen.insert(entry.str).second) result.push_back(entry.str);
    }
    return result;
}

/** @param text 原始 workspace 文本。@param deleted 仅包含实际清理成功的 ID。@return 仅替换归档数组后的完整文本。 */
static std::string RewriteArchivedIds(const std::string& text, const std::set<std::string>& deleted)
{
    // 单独解析以取得当前文本的精确数组字节范围。
    Json document;
    // 解析器与本次 text 一一对应。
    JsonParser parser(text);
    if (!parser.parse(document)) throw CString(L"归档索引格式已变化，无法安全更新。");
    // 必须更新 global 下的数组，其他同名字段保持原样。
    const Json& archived = ArchivedArray(document);
    // 新数组只拼接原始合法节点，不自行拼接未转义的会话 ID。
    std::string array = "[";
    // entry 表示当前原始数组条目；未删除项的原始转义保持不变。
    for (const Json& entry : archived.arr)
    {
        if (entry.type != Json::STR || !IsSessionId(entry.str)) throw CString(L"归档索引含有非法会话 ID。");
        if (deleted.count(entry.str)) continue;
        if (array.size() > 1) array += ',';
        array += text.substr(entry.begin, entry.end - entry.begin);
    }
    array += ']';
    return text.substr(0, archived.begin) + array + text.substr(archived.end);
}

/** @param text 原始 projcache 文本。@param deleted 仅包含清理成功的 ID。@return 只移除对应缓存记录后的文本。 */
static std::string RewriteCache(const std::string& text, const std::set<std::string>& deleted)
{
    // 缓存同样必须完整解析，不能为清除少量记录而删除所有会话缓存。
    Json document;
    // 当前缓存文本的解析器。
    JsonParser parser(text);
    if (!parser.parse(document) || document.type != Json::OBJ) throw CString(L"会话缓存 JSON 格式错误，已停止操作。");
    // 缓存表集合由根对象持有。
    const Json* tables = document.get("tables");
    // 会话缓存表可能不存在；不存在时无需更新。
    const Json* sessions = tables ? tables->get("sessions") : nullptr;
    if (!sessions) return text;
    if (sessions->type != Json::OBJ) throw CString(L"无法识别会话缓存表，已停止操作。");
    // 缓存对象的替换内容，未删除记录的值直接沿用原始字节。
    std::string object = "{";
    // member 表示解析后的一个缓存成员；未删除值的原始字节保持不变。
    for (const auto& member : sessions->obj)
    {
        if (deleted.count(member.first)) continue;
        // 成员名通过单独的标准 JSON 转义写出，值不经过重新序列化。
        std::string key = "\"";
        // byte 表示成员名的一个 UTF-8 字节；控制字节使用四位 Unicode 转义。
        for (const unsigned char byte : member.first)
        {
            if (byte == '"' || byte == '\\') { key += '\\'; key += (char)byte; }
            else if (byte < 0x20)
            {
                // 小于 0x20 的字节可精确编码为六个 ASCII 转义字节。
                char escape[7]{};
                sprintf_s(escape, "\\u%04x", byte);
                key += escape;
            }
            else key += (char)byte;
        }
        key += '"';
        if (object.size() > 1) object += ',';
        object += key + ':' + text.substr(member.second.begin, member.second.end - member.second.begin);
    }
    object += '}';
    return text.substr(0, sessions->begin) + object + text.substr(sessions->end);
}

/** @param path 将要替换的已有文件。@throws CString 文件只读、被锁定或缺少替换权限时拒绝开始删除。 */
static void CheckReplaceable(const fs::path& path)
{
    if (!IsPlainLocalPath(path)) throw CString(L"索引或缓存经过链接，已停止操作。");
    // 只读属性必须在删除任何目录之前检查。
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)))
        throw CString(L"索引或缓存只读、缺失或不是普通文件，已停止操作。");
    // 以替换所需的权限探测占用情况，探测后立即关闭，不长期锁定应用数据。
    const HANDLE probe = CreateFileW(path.c_str(), GENERIC_READ | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (probe == INVALID_HANDLE_VALUE) throw CString(L"索引或缓存被占用或没有替换权限，请先退出 DeepSeek Harness。");
    CloseHandle(probe);
}

/** @param path 目标普通本地文件。@param text 完整 UTF-8 内容。@throws CString 写入或替换失败；不截断原文件。 */
static void AtomicWrite(const fs::path& path, const std::string& text)
{
    if (!IsPlainLocalPath(path)) throw CString(L"拒绝向链接或不安全路径写入文件。");
    // 同目录临时文件采用进程号和递增序号，CREATE_NEW 防止覆盖已有文件。
    static unsigned sequence = 0;
    // 临时路径仅由本进程构造，不使用索引里的会话名称。
    fs::path temporary = path;
    temporary += L".sessioncleaner-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence) + L".tmp";
    // 本函数拥有的文件句柄，完成写入和磁盘刷新后关闭。
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw CString(L"无法创建同目录临时文件，未开始更新索引。");
    // 实际写入长度必须与 UTF-8 字节数完全一致。
    DWORD written = 0;
    // 先完整写入并刷新，再替换目录项，防止直接截断 workspace.json。
    const bool success = (text.empty() || WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr))
        && written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!success || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        throw CString(L"文件替换失败；原始索引备份仍保留在数据目录的 .bak 文件中。");
    }
}

/** @return 检测到 DeepSeek Harness 或无法完成进程检查时返回 true，以保守地阻止删除。 */
static bool IsDshRunning()
{
    // 进程快照句柄由本函数关闭，不读取其他进程的内存或命令行。
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;
    // 只使用可执行文件名判断已知的 DSH 进程。
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    // 快照无法枚举时也阻止破坏性操作。
    bool found = !Process32FirstW(snapshot, &process);
    if (!found)
    {
        do
        {
            if (_wcsicmp(process.szExeFile, L"DeepSeek Harness.exe") == 0) { found = true; break; }
        } while (Process32NextW(snapshot, &process));
    }
    CloseHandle(snapshot);
    return found;
}

/** @param root 已校验的数据根目录。@param id 安全会话 ID。@return 唯一匹配的目录；确实不存在时返回空路径。 */
static fs::path FindSessionDirectory(const fs::path& root, const std::string& id)
{
    if (!IsSessionId(id)) throw CString(L"不安全的会话 ID，已停止操作。");
    // 所有会话只能位于根目录的 sessions 下，cwd 不参与定位。
    const fs::path sessions = root / L"sessions";
    if (!IsPlainLocalPath(sessions)) throw CString(L"会话目录经过链接或联接，已停止操作。");
    // 枚举和存在性检查的错误必须与目录缺失区分。
    std::error_code error;
    // 缺失 sessions 允许清理纯索引残留，权限错误则阻止操作。
    const bool exists = fs::exists(sessions, error);
    if (error) throw CString(L"无法检查会话目录，已停止操作。");
    if (!exists) return fs::path();
    // 唯一匹配结果，不接受同一 ID 对应多个工作区目录。
    fs::path result;
    // 不使用 skip_permission_denied，防止把不可访问目录误判为不存在。
    fs::directory_iterator workspace(sessions, error);
    if (error) throw CString(L"无法枚举会话工作区，已停止操作。");
    // workspace 指向每个直接工作区，不递归搜索其他位置。
    for (; workspace != fs::directory_iterator(); workspace.increment(error))
    {
        if (error) throw CString(L"会话工作区枚举失败，已停止操作。");
        if (!IsPlainLocalPath(workspace->path())) throw CString(L"工作区存在链接或无法安全检查，已停止操作。");
        if (!workspace->is_directory(error))
        {
            if (error) throw CString(L"无法检查工作区属性。");
            continue;
        }
        // 拼接经过白名单校验的单层名称，不能包含路径跳转。
        const fs::path candidate = workspace->path() / fs::path(FromUtf8(id).GetString());
        if (!IsPlainLocalPath(candidate)) throw CString(L"会话路径存在链接或无法安全检查，已停止操作。");
        if (!fs::exists(candidate, error))
        {
            if (error) throw CString(L"无法检查会话目录是否存在。");
            continue;
        }
        if (!fs::is_directory(candidate, error) || error) throw CString(L"会话路径不是可访问的普通目录。");
        if (!result.empty()) throw CString(L"同一会话 ID 对应多个目录，请先检查工作区数据。");
        result = candidate;
    }
    if (error) throw CString(L"会话工作区枚举未完成，已停止操作。");
    return result;
}

/** @param directory 普通会话目录。@return 不经过任何重解析点的文件总字节数；检查失败时抛出 CString。 */
static ULONGLONG DirSize(const fs::path& directory)
{
    if (!IsPlainLocalPath(directory)) throw CString(L"会话目录不安全，已停止操作。");
    // 聚合实际文件长度，不读取会话正文。
    ULONGLONG total = 0;
    // 文件系统错误通过显式参数处理，不略过无权限的条目。
    std::error_code error;
    // 默认不跟随目录符号链接；同时主动拒绝遇到的所有重解析点。
    fs::recursive_directory_iterator entry(directory, error);
    if (error) throw CString(L"无法读取会话目录内容。");
    for (; entry != fs::recursive_directory_iterator(); entry.increment(error))
    {
        if (error) throw CString(L"会话目录未能完整检查。");
        // 拒绝符号链接文件、目录联接和云占位重解析文件，避免管理员权限越界访问。
        const DWORD attributes = GetFileAttributesW(entry->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw CString(L"会话目录内含链接或无法检查的文件，已停止操作。");
        if (entry->is_regular_file(error))
        {
            // 单文件长度以字节为单位，查询失败时不得使用无效的最大整数。
            const ULONGLONG bytes = entry->file_size(error);
            if (error) throw CString(L"无法统计会话文件大小。");
            total += bytes;
        }
        if (error) throw CString(L"无法检查会话文件类型。");
    }
    if (error) throw CString(L"会话目录未能完整检查。");
    return total;
}

/** @param directory 普通会话目录。@return 目录最后写入时间，失败时为零 FILETIME。 */
static FILETIME DirMTime(const fs::path& directory)
{
    // 系统返回的目录属性，生命周期限于本次调用。
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    return GetFileAttributesExW(directory.c_str(), GetFileExInfoStandard, &attributes)
        ? attributes.ftLastWriteTime : FILETIME{};
}

/** @param bytes 文件长度，单位为字节。@return 面向界面的 KB、MB 或 GB 文本。 */
static CString FormatSize(ULONGLONG bytes)
{
    // 临时格式化字符串由返回值持有。
    CString text;
    if (bytes >= 1024ull * 1024 * 1024) text.Format(L"%.2f GB", bytes / 1073741824.0);
    else if (bytes >= 1024ull * 1024) text.Format(L"%.2f MB", bytes / 1048576.0);
    else text.Format(L"%.1f KB", bytes / 1024.0);
    return text;
}

/** @param time Windows UTC 文件时间。@return 本地时间的年月日时分；未知时间返回短横线。 */
static CString FormatTime(const FILETIME& time)
{
    if (!time.dwLowDateTime && !time.dwHighDateTime) return L"-";
    // 转换后的本地文件时间，仍使用 FILETIME 单位。
    FILETIME local{};
    // 用于展示的本地日历字段。
    SYSTEMTIME calendar{};
    if (!FileTimeToLocalFileTime(&time, &local) || !FileTimeToSystemTime(&local, &calendar)) return L"-";
    // 只在本地界面显示，不写入诊断文件。
    CString text;
    text.Format(L"%04d-%02d-%02d %02d:%02d", calendar.wYear, calendar.wMonth,
                calendar.wDay, calendar.wHour, calendar.wMinute);
    return text;
}

/** @param encoded 工作区目录的编码名称。@return 仅用于展示的解码路径，绝不作为删除地址。 */
static CString DecodeWorkspaceName(const CString& encoded)
{
    // 去掉工作区名称的外层标记，不触碰磁盘。
    CString source = encoded;
    if (source.Left(2) == L"--") source = source.Mid(2);
    if (source.Right(2) == L"--") source = source.Left(source.GetLength() - 2);
    // 解码结果用于标题和 cwd 的本地兜底。
    CString output;
    // index 表示当前 UTF-16 码元位置，~XXXX 视为一个转义单元。
    for (int index = 0; index < source.GetLength(); ++index)
    {
        if (source[index] == L'-') { output += L'\\'; continue; }
        if (source[index] == L'~' && index + 4 < source.GetLength())
        {
            // 四位编码文本只在当前迭代期间使用。
            const CString hex = source.Mid(index + 1, 4);
            // 解析终点用于拒绝部分匹配和非十六进制后缀。
            wchar_t* end = nullptr;
            // 工作区转义表示单个 UTF-16 码元。
            const long value = wcstol(hex, &end, 16);
            if (end == hex.GetString() + 4) { output += (wchar_t)value; index += 4; continue; }
        }
        output += source[index];
    }
    // 工作区编码把盘符冒号省略了，展示时补回，不能把 C\Users 误显示成相对路径。
    if (output.GetLength() >= 2 && output[1] == L'\\' &&
        ((output[0] >= L'A' && output[0] <= L'Z') || (output[0] >= L'a' && output[0] <= L'z')))
        output.Insert(1, L':');
    return output;
}

/** @return 归档会话的本地展示快照；索引损坏或路径不安全时抛出 CString，不伪装为空列表。 */
static std::vector<SessionItem> ScanSessions()
{
    // 已验证的数据根目录，只用于本轮扫描。
    const fs::path root = DataRoot();
    // 仅扫描归档数组，不枚举未归档会话 ID。
    const std::vector<std::string> ids = ParseArchivedIds(ReadAllUtf8(root / L"storages" / L"workspace.json"));
    // 可选标题缓存，解析失败时仅放弃展示缓存，不会修改缓存文件。
    Json cache;
    // 会话缓存表指向 cache 内部，有效期覆盖当前扫描。
    const Json* sessions = nullptr;
    // 缓存文件路径，不包含令牌、密码或网络地址。
    const fs::path cachePath = root / L"storages" / L"session_projcache.json";
    if (GetFileAttributesW(cachePath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        // 源文本保持到解析完成，扫描只使用解码后的节点内容。
        const std::string text = ReadAllUtf8(cachePath);
        // 缓存读取不参与 workspace 语义校验。
        JsonParser parser(text);
        if (parser.parse(cache))
        {
            // 可选的表集合，仅用于查找会话标题和工作目录。
            const Json* tables = cache.get("tables");
            sessions = tables ? tables->get("sessions") : nullptr;
        }
    }
    // 返回给界面的快照由调用者持有。
    std::vector<SessionItem> items;
    // id 是已经过白名单验证的归档 ID。
    for (const std::string& id : ids)
    {
        // 单个会话的本地展示记录。
        SessionItem item;
        item.id = FromUtf8(id);
        // 缓存记录只提供展示文字，不影响删除路径。
        const Json* record = sessions ? sessions->get(id.c_str()) : nullptr;
        if (record)
        {
            // 可选的标题表，仅在本次记录解析中借用。
            const Json* rows = record->get("rows");
            // 标题值位于 title.val，缺失时由界面兜底。
            const Json* title = rows ? rows->get("title") : nullptr;
            if (title) item.title = FromUtf8(title->s("val"));
            // 工作目录仅显示，不会读取该目录的内容。
            const Json* identity = record->get("identity");
            if (identity) item.cwd = FromUtf8(identity->s("cwd"));
        }
        // 重新从 sessions 的直接工作区中定位唯一普通目录。
        const fs::path directory = FindSessionDirectory(root, id);
        if (!directory.empty())
        {
            item.path = directory.c_str();
            item.workspace = directory.parent_path().filename().c_str();
            item.bytes = DirSize(directory);
            item.mtime = DirMTime(directory);
        }
        items.push_back(std::move(item));
    }
    return items;
}

/**
 * 执行已选归档的文件事务；调用者必须先完成进程运行状态检查。
 * @param items 用户确认后的展示快照；实际路径和归档状态会重新检查。
 * @param okCount 输出已清理的归档条目数，包含确认缺失目录的残留索引。
 * @param failCount 输出目录删除失败的条目数；其索引和缓存保留。
 * @throws CString 格式错误、备份失败或元数据写入失败。
 */
static void DeleteArchivedFiles(const std::vector<SessionItem>& items, int& okCount, int& failCount)
{
    okCount = 0;
    failCount = 0;
    if (items.empty()) return;
    // 所有元数据和目标路径均基于本轮验证的根目录。
    const fs::path root = DataRoot();
    // 必须更新的归档索引文件。
    const fs::path indexPath = root / L"storages" / L"workspace.json";
    // 可选缓存文件只做定向记录移除。
    const fs::path cachePath = root / L"storages" / L"session_projcache.json";
    // 原始索引同时用于备份和提交前的并发修改检查。
    const std::string indexText = ReadAllUtf8(indexPath);
    // 当前归档集合用于防止扫描之后已经取消归档的会话被删除。
    const std::vector<std::string> archived = ParseArchivedIds(indexText);
    // 集合查询不改变原始数组顺序。
    const std::set<std::string> archivedSet(archived.begin(), archived.end());
    // 已验证的目标表，空路径代表确认不存在的目录。
    std::map<std::string, fs::path> targets;
    // item 来自用户界面，但 ID 和路径均不能直接信任。
    for (const SessionItem& item : items)
    {
        // 只允许仍在当前归档数组中的安全标识符。
        const std::string id = ToUtf8(item.id);
        if (!IsSessionId(id) || !archivedSet.count(id)) throw CString(L"选中的会话归档状态已变化，请先刷新列表。");
        // 删除路径在此重新定位，不使用展示快照中的 path 或 cwd。
        const fs::path directory = FindSessionDirectory(root, id);
        if (!directory.empty()) DirSize(directory);
        targets.emplace(id, directory);
    }
    // 缓存不存在可以继续，但访问失败不能被当作缺失。
    const DWORD cacheAttributes = GetFileAttributesW(cachePath.c_str());
    // 存在性判断要同时记住错误码，后续 API 会覆盖 GetLastError。
    const DWORD cacheError = cacheAttributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    if (cacheAttributes == INVALID_FILE_ATTRIBUTES && cacheError != ERROR_FILE_NOT_FOUND && cacheError != ERROR_PATH_NOT_FOUND)
        throw CString(L"无法检查会话缓存，已停止操作。");
    // 原始缓存也保留在内存中，既用于定向重写，也用于安全备份。
    const std::string cacheText = cacheAttributes != INVALID_FILE_ATTRIBUTES ? ReadAllUtf8(cachePath) : std::string();
    if (cacheAttributes != INVALID_FILE_ATTRIBUTES) RewriteCache(cacheText, {});
    CheckReplaceable(indexPath);
    if (cacheAttributes != INVALID_FILE_ATTRIBUTES) CheckReplaceable(cachePath);
    // 任何备份失败都在删除目录之前中止；备份通过临时文件替换，不截断旧备份。
    AtomicWrite(root / L"storages" / L"workspace.json.bak", indexText);
    if (cacheAttributes != INVALID_FILE_ATTRIBUTES)
        AtomicWrite(root / L"storages" / L"session_projcache.json.bak", cacheText);
    // 只把实际删除成功或确认不存在的 ID 加入提交集合。
    std::set<std::string> deleted;
    // target 是本轮重新校验的安全 ID 与目录配对。
    for (const auto& target : targets)
    {
        if (!target.second.empty())
        {
            // 删除前再次拒绝目录内出现的链接；失败时保留该条目的元数据。
            try { DirSize(target.second); }
            catch (const CString&) { ++failCount; continue; }
            // 不抛出的错误参数让单个失败目录不影响其他条目的结果统计。
            std::error_code error;
            fs::remove_all(target.second, error);
            if (error) { ++failCount; continue; }
        }
        deleted.insert(target.first);
    }
    if (deleted.empty()) return;
    // 提交前检查索引和缓存是否被外部进程改动，避免覆盖其他人的新内容。
    if (ReadAllUtf8(indexPath) != indexText ||
        (cacheAttributes != INVALID_FILE_ATTRIBUTES && ReadAllUtf8(cachePath) != cacheText))
        throw CString(L"清理期间数据文件被其他程序修改；部分目录可能已删除，请退出 DSH 后刷新，元数据备份已保留。");
    // 先更新缓存再提交索引；任一步失败都明确报告，不能声称清理完整成功。
    if (cacheAttributes != INVALID_FILE_ATTRIBUTES) AtomicWrite(cachePath, RewriteCache(cacheText, deleted));
    AtomicWrite(indexPath, RewriteArchivedIds(indexText, deleted));
    okCount = (int)deleted.size();
}

/**
 * 图形界面和命令行共用的删除入口，进程保护不能由用户参数关闭。
 * @param items 用户选择的归档快照。
 * @param okCount 输出成功清理的索引条目数。
 * @param failCount 输出目录删除失败且保留元数据的条目数。
 * @throws CString DSH 正在运行、无法检查进程或底层文件事务失败。
 */
static void DoDelete(const std::vector<SessionItem>& items, int& okCount, int& failCount)
{
    okCount = 0;
    failCount = 0;
    if (IsDshRunning()) throw CString(L"DeepSeek Harness 正在运行或无法检查进程，请先完全退出后重试。");
    DeleteArchivedFiles(items, okCount, failCount);
}
