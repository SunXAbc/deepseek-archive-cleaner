#include "../SessionCleanerCore.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

// MFC 控制台测试需要唯一的应用对象，但不创建任何窗口或读取默认用户数据。
CWinApp g_testApplication;
// 已执行的断言总数，用于汇总回归结果。
static unsigned g_assertions = 0;
// 运行器预先创建的临时父目录，使受限测试进程也只在获准的夹具范围内写入。
static fs::path g_fixtureBase;

/** @param condition 必须成立的测试条件。@param message 失败时的固定英文说明，不包含用户数据。 */
static void Require(bool condition, const char* message)
{
    ++g_assertions;
    if (!condition) throw std::runtime_error(message);
}

/** @param path 测试拥有的文件路径。@param text 要写入的测试字节；不使用生产写入函数创建样本。 */
static void Put(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    // 测试文件由标准库流独立写入，以免生产写入错误同时污染测试夹具。
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), (std::streamsize)text.size());
    if (!file) throw std::runtime_error("fixture write failed");
}

/** 一个测试拥有一个全新的临时数据根目录，析构只清理自身创建的范围。 */
class Fixture
{
public:
    // 当前样本根目录，所有测试数据均在系统临时目录下。
    fs::path root;

    /** 创建独立夹具并把生产根目录覆盖限定到本样本；无参数。 */
    Fixture()
    {
        if (g_fixtureBase.empty() || !g_fixtureBase.is_absolute()) throw std::runtime_error("no fixture base");
        // 序号用于避免同一毫秒内创建的测试目录相互覆盖。
        static unsigned sequence = 0;
        root = g_fixtureBase / (L"DeepSeekCleaner-test-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence));
        fs::create_directories(root / L"storages");
        g_rootOverride = root.c_str();
    }

    /** 仅清理本类直接创建的临时目录，不跟随外部夹具或真实用户目录。 */
    ~Fixture()
    {
        // 清理前再次验证绝对路径和固定前缀，避免测试失败后错误地扩大范围。
        if (root.is_absolute() && root.parent_path().lexically_normal() == g_fixtureBase.lexically_normal()
            && root.filename().wstring().find(L"DeepSeekCleaner-test-") == 0)
        {
            // 清理只用于自建夹具，忽略清理错误以免析构期间抛出异常。
            std::error_code error;
            fs::remove_all(root, error);
        }
        g_rootOverride.Empty();
    }

    /** @param ids 数组内部的测试 JSON 文本，例如双引号括起的会话 ID。 */
    void Index(const std::string& ids)
    {
        Put(root / L"storages" / L"workspace.json",
            "{\"unit\":\"test\",\"global\":{\"archivedSessionIds\":[" + ids
            + "],\"pinnedSessionIds\":[\"session-active\"]},\"tables\":{\"keep\":true}}");
    }

    /** @param id 虚构会话 ID。@return 新建会话目录的绝对路径。 */
    fs::path Session(const std::string& id)
    {
        // 所有目录都放在单一虚构工作区，便于验证目标边界。
        const fs::path directory = root / L"sessions" / L"test-workspace" / fs::path(FromUtf8(id).GetString());
        Put(directory / L"sample.txt", "synthetic content only");
        return directory;
    }

    /** @param text 完整的虚构缓存 JSON 文本。 */
    void Cache(const std::string& text)
    {
        Put(root / L"storages" / L"session_projcache.json", text);
    }
};

/** @param operation 预期抛出 CString 的生产操作。@return 是否按预期拒绝。 */
template<class Operation>
static bool Rejects(Operation operation)
{
    try { operation(); }
    catch (const CString&) { return true; }
    return false;
}

/** 验证残缺、重复键、非法数字和 Unicode 转义都不会进入破坏性流程。 */
static void TestStrictJson()
{
    // 全部样本都是内存中的虚构文本，不读取用户文件。
    const char* invalid[] = { "{} trailing", "{\"a\":1,\"a\":2}", "[1,]", "{\"a\":}",
        "+1", "01", "1.", "1e", "\"\\uD800\"", "\"\\uDC00\"", "\"\\uZZZZ\"", "\"a\nb\"" };
    // text 表示当前应被拒绝的完整 JSON 文本。
    for (const char* text : invalid)
    {
        // 节点必须在每个样本之间重新初始化。
        Json parsed;
        // 解析器借用本次样本文本。
        const std::string source(text);
        Require(!JsonParser(source).parse(parsed), "malformed JSON accepted");
    }
    // 合法代理对和 BOM 应正常解码，不影响源范围。
    const std::string valid = "\xEF\xBB\xBF{\"emoji\":\"\\uD83D\\uDE00\",\"number\":-1.25e+3}";
    // 合法样本的解析结果。
    Json parsed;
    Require(JsonParser(valid).parse(parsed), "valid BOM or surrogate rejected");
    Require(parsed.s("emoji") == "\xF0\x9F\x98\x80", "surrogate decode incorrect");
}

/** 验证只识别 global 下的归档数组，更新时保留同名字段、BOM 和未删除条目的转义。 */
static void TestScopedIndex()
{
    // 其他对象和正文包含同名字段，不能被误当作归档数组。
    const std::string source = "\xEF\xBB\xBF{\"other\":{\"archivedSessionIds\":[\"session-other\"]},"
        "\"global\":{\"archivedSessionIds\":[\"session-a\",\"session-a\",\"session-\\u0062\"]},\"keep\":42}";
    // 扫描应去重，但不能丢失另一条合法 ID。
    const auto ids = ParseArchivedIds(source);
    Require(ids.size() == 2 && ids[1] == "session-b", "scoped parsing or deduplication failed");
    // 仅删除 a，保留其他对象和 b 的原始转义文本。
    const std::string rewritten = RewriteArchivedIds(source, { "session-a" });
    Require(rewritten.find("\"other\":{\"archivedSessionIds\":[\"session-other\"]}") != std::string::npos, "unrelated array changed");
    Require(rewritten.find("session-\\u0062") != std::string::npos, "remaining string escape changed");
    Require(rewritten.compare(0, 3, "\xEF\xBB\xBF") == 0, "BOM lost");
    Require(ParseArchivedIds(rewritten).size() == 1, "duplicate ID not fully removed");
    Require(Rejects([] { ParseArchivedIds("{\"archivedSessionIds\":[]}"); }), "wrong schema accepted");
}

/** 验证路径跳转、盘符、空字节及非字符串 ID 均被拒绝。 */
static void TestUnsafeIds()
{
    // 这些标识符都不能用于拼接文件系统路径。
    const char* unsafe[] = { "session-../outside", "session-C:secret", "session-..\\outside", "session-", "other-id" };
    // id 表示当前不安全标识符。
    for (const char* id : unsafe) Require(!IsSessionId(id), "unsafe path identifier accepted");
    Require(Rejects([] { ParseArchivedIds("{\"global\":{\"archivedSessionIds\":[42]}}"); }), "non-string ID accepted");
    Require(Rejects([] { ParseArchivedIds("{\"global\":{\"archivedSessionIds\":[\"session-a\\u0000b\"]}}"); }), "NUL ID accepted");
}

/** 验证选中项删除、未选项保留、非归档目录不受影响，以及备份包含操作前的精确字节。 */
static void TestSelectedDeletion()
{
    // 独立的合成数据目录。
    Fixture fixture;
    fixture.Index("\"session-a\",\"session-b\"");
    // 待删除的归档目录。
    const fs::path removed = fixture.Session("session-a");
    // 未勾选的归档目录必须保留。
    const fs::path kept = fixture.Session("session-b");
    // 未归档目录绝不作为扫描或删除目标。
    const fs::path active = fixture.Session("session-active");
    fixture.Cache("{\"tables\":{\"sessions\":{\"session-a\":{\"secret\":\"fake-a\"},\"session-b\":{\"keep\":2},"
        "\"session-active\":{\"keep\":3}},\"other\":{\"keep\":4}}}");
    // 操作前索引与缓存的精确字节用于备份核验。
    const std::string index = ReadAllUtf8(fixture.root / L"storages" / L"workspace.json");
    // 操作前缓存文本，包含未归档会话。
    const std::string cache = ReadAllUtf8(fixture.root / L"storages" / L"session_projcache.json");
    // 扫描只应返回两条归档记录。
    const auto items = ScanSessions();
    Require(items.size() == 2, "active session included in scan");
    // 成功提交的归档条目数。
    int ok = 0;
    // 删除失败的归档条目数。
    int failed = 0;
    DeleteArchivedFiles({ items[0] }, ok, failed);
    Require(ok == 1 && failed == 0, "selected deletion count incorrect");
    Require(!fs::exists(removed) && fs::exists(kept) && fs::exists(active), "deletion escaped selection");
    Require(ReadAllUtf8(fixture.root / L"storages" / L"workspace.json.bak") == index, "index backup changed bytes");
    Require(ReadAllUtf8(fixture.root / L"storages" / L"session_projcache.json.bak") == cache, "cache backup changed bytes");
    // 修改后的缓存只允许移除选中且清理成功的记录。
    const std::string updated = ReadAllUtf8(fixture.root / L"storages" / L"session_projcache.json");
    Require(updated.find("session-a\"") == std::string::npos && updated.find("session-b") != std::string::npos
        && updated.find("session-active") != std::string::npos && updated.find("\"other\":{\"keep\":4}") != std::string::npos,
        "unrelated cache records lost");
}

/** 验证目录确实不存在时可以只清理残留索引，并准确计入成功数量。 */
static void TestMissingDirectory()
{
    // 没有 sessions 目录的独立夹具。
    Fixture fixture;
    fixture.Index("\"session-missing\"");
    // 缺失目录仍应显示一条可解释的残留索引。
    const auto items = ScanSessions();
    Require(items.size() == 1 && items[0].path.IsEmpty(), "missing directory state incorrect");
    // 输出成功计数。
    int ok = 0;
    // 输出失败计数。
    int failed = 0;
    DeleteArchivedFiles(items, ok, failed);
    Require(ok == 1 && failed == 0 && ScanSessions().empty(), "missing index cleanup failed");
}

/** 验证文件被锁定时，删除失败的 ID 和缓存记录仍然保留。 */
static void TestLockedSession()
{
    // 只有一个待清理会话的独立夹具。
    Fixture fixture;
    fixture.Index("\"session-locked\"");
    // 被占用文件所属的普通目录。
    const fs::path directory = fixture.Session("session-locked");
    fixture.Cache("{\"tables\":{\"sessions\":{\"session-locked\":{\"keep\":true}}}}");
    // 删除前扫描，随后通过 Windows 句柄制造真实共享冲突。
    const auto items = ScanSessions();
    // 不允许 FILE_SHARE_DELETE 的句柄由本函数持有。
    const HANDLE locked = CreateFileW((directory / L"sample.txt").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(locked != INVALID_HANDLE_VALUE, "could not lock fixture");
    // 成功计数必须保持零。
    int ok = 0;
    // 失败计数应为一。
    int failed = 0;
    DeleteArchivedFiles(items, ok, failed);
    CloseHandle(locked);
    Require(ok == 0 && failed == 1 && fs::exists(directory), "locked deletion reported success");
    Require(ParseArchivedIds(ReadAllUtf8(fixture.root / L"storages" / L"workspace.json")).size() == 1, "failed ID removed");
    Require(ReadAllUtf8(fixture.root / L"storages" / L"session_projcache.json").find("session-locked") != std::string::npos,
        "failed cache record removed");
}

/** 验证同一批次部分成功时只提交成功项，失败条目的索引和缓存均保留。 */
static void TestPartialDeletion()
{
    // 锁定和可删除目录同时出现在本次选择中。
    Fixture fixture;
    fixture.Index("\"session-locked\",\"session-free\"");
    // 不能被删除的会话目录。
    const fs::path lockedDirectory = fixture.Session("session-locked");
    // 应正常删除的另一会话目录。
    const fs::path freeDirectory = fixture.Session("session-free");
    fixture.Cache("{\"tables\":{\"sessions\":{\"session-locked\":{\"keep\":1},\"session-free\":{\"remove\":2}}}}");
    // 使用真实 Windows 共享锁触发单条删除失败。
    const HANDLE locked = CreateFileW((lockedDirectory / L"sample.txt").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(locked != INVALID_HANDLE_VALUE, "could not lock mixed fixture");
    // 成功提交数量应为一。
    int ok = 0;
    // 失败并保留元数据的数量应为一。
    int failed = 0;
    DeleteArchivedFiles(ScanSessions(), ok, failed);
    CloseHandle(locked);
    Require(ok == 1 && failed == 1 && fs::exists(lockedDirectory) && !fs::exists(freeDirectory), "partial deletion count incorrect");
    // 只有失败项留在最新归档索引中。
    const auto remaining = ParseArchivedIds(ReadAllUtf8(fixture.root / L"storages" / L"workspace.json"));
    Require(remaining.size() == 1 && remaining[0] == "session-locked", "partial failure index corrupted");
    // 缓存定向移除成功项，保留失败项。
    const std::string cache = ReadAllUtf8(fixture.root / L"storages" / L"session_projcache.json");
    Require(cache.find("session-locked") != std::string::npos && cache.find("session-free") == std::string::npos,
        "partial failure cache corrupted");
}

/** 验证备份路径不可写时，在删除任何会话之前中止。 */
static void TestBackupFailure()
{
    // 用目录占据备份文件名，稳定复现文件替换失败。
    Fixture fixture;
    fixture.Index("\"session-a\"");
    // 原会话文件必须完整保留。
    const fs::path directory = fixture.Session("session-a");
    fs::create_directories(fixture.root / L"storages" / L"workspace.json.bak");
    // 成功计数即使异常也不能误报。
    int ok = 0;
    // 失败计数由生产操作初始化。
    int failed = 0;
    Require(Rejects([&] { DeleteArchivedFiles(ScanSessions(), ok, failed); }), "backup failure ignored");
    Require(fs::exists(directory / L"sample.txt") && ScanSessions().size() == 1, "data deleted before backup");
}

/** 验证索引只读、被锁定或缓存损坏时，目标内容不变。 */
static void TestMetadataFailure()
{
    // 所有故障在同一夹具中顺序构造，每次都验证内容仍在。
    Fixture fixture;
    fixture.Index("\"session-a\"");
    // 索引拒写时仍必须存在的会话目录。
    const fs::path directory = fixture.Session("session-a");
    // 当前索引路径，属性会在测试结束前恢复。
    const fs::path index = fixture.root / L"storages" / L"workspace.json";
    // 扫描快照用于验证后续元数据检查，不依赖再次扫描是否成功。
    const auto items = ScanSessions();
    // 成功计数输出。
    int ok = 0;
    // 失败计数输出。
    int failed = 0;
    SetFileAttributesW(index.c_str(), FILE_ATTRIBUTE_READONLY);
    Require(Rejects([&] { DeleteArchivedFiles(items, ok, failed); }), "read-only index accepted");
    SetFileAttributesW(index.c_str(), FILE_ATTRIBUTE_NORMAL);
    // 允许读取但不允许删除/替换，复现索引被其他进程占用。
    const HANDLE locked = CreateFileW(index.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(locked != INVALID_HANDLE_VALUE, "could not lock index");
    Require(Rejects([&] { DeleteArchivedFiles(items, ok, failed); }), "locked index accepted");
    CloseHandle(locked);
    fixture.Cache("{ broken");
    Require(Rejects([&] { DeleteArchivedFiles(items, ok, failed); }), "malformed cache accepted");
    Require(fs::exists(directory / L"sample.txt"), "metadata failure deleted content");
}

/** 验证扫描后取消归档或目录出现歧义时，旧快照不能继续删除。 */
static void TestStaleSelection()
{
    // 独立的扫描和状态变化样本。
    Fixture fixture;
    fixture.Index("\"session-a\"");
    // 原始会话目录。
    const fs::path directory = fixture.Session("session-a");
    // 在状态变化之前取得的选择快照。
    const auto items = ScanSessions();
    fixture.Index("");
    // 输出成功计数。
    int ok = 0;
    // 输出失败计数。
    int failed = 0;
    Require(Rejects([&] { DeleteArchivedFiles(items, ok, failed); }), "stale archived state accepted");
    fixture.Index("\"session-a\"");
    Put(fixture.root / L"sessions" / L"other-workspace" / L"session-a" / L"sample.txt", "other synthetic directory");
    Require(Rejects([&] { DeleteArchivedFiles(items, ok, failed); }), "ambiguous directories accepted");
    Require(fs::exists(directory / L"sample.txt"), "stale or ambiguous selection deleted data");
}

/** 验证恶意修改展示快照的 path 和 cwd 不能改变实际删除目标。 */
static void TestSnapshotPathIgnored()
{
    // 本轮操作的独立合成数据。
    Fixture fixture;
    fixture.Index("\"session-a\"");
    fixture.Session("session-a");
    // 假装是外部工作目录的无关文件，位于测试夹具内便于安全清理。
    const fs::path unrelated = fixture.root / L"unrelated";
    Put(unrelated / L"keep.txt", "must survive");
    // 扫描快照被故意注入与会话目录无关的路径。
    auto items = ScanSessions();
    items[0].path = unrelated.c_str();
    items[0].cwd = unrelated.c_str();
    // 输出成功条目数。
    int ok = 0;
    // 输出失败条目数。
    int failed = 0;
    DeleteArchivedFiles(items, ok, failed);
    Require(ok == 1 && fs::exists(unrelated / L"keep.txt"), "snapshot path used as deletion target");
}

/** @param root 由 PowerShell 运行器创建的联接测试根目录。验证外部标记始终不被读取或删除。 */
static void TestJunctions(const fs::path& root)
{
    g_rootOverride = (root / L"junction").c_str();
    Require(Rejects([] { ScanSessions(); }), "workspace junction accepted");
    g_rootOverride = (root / L"inner").c_str();
    Require(Rejects([] { ScanSessions(); }), "nested junction accepted");
    Require(fs::exists(root / L"outside" / L"keep.txt"), "junction target changed");
    g_rootOverride.Empty();
}

/** 验证运行中的 DSH 会在核心函数中阻止删除，不依赖图形界面检查。 */
static void TestProcessGuard()
{
    Require(IsDshRunning(), "synthetic DSH process not detected");
    // 本测试只使用自己的临时归档数据。
    Fixture fixture;
    fixture.Index("\"session-a\"");
    // 应因运行中保护而保持完整的目录。
    const fs::path directory = fixture.Session("session-a");
    // 输出成功条目数。
    int ok = 0;
    // 输出失败条目数。
    int failed = 0;
    Require(Rejects([&] { DoDelete(ScanSessions(), ok, failed); }), "running process did not block core deletion");
    Require(fs::exists(directory / L"sample.txt"), "process guard deleted data");
}

/** @param argc 参数数量。@param argv 接受夹具路径、--guard 加夹具路径或 --hold。@return 0 表示全部通过，1 表示失败。 */
int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && wcscmp(argv[1], L"--hold") == 0) { Sleep(30000); return 0; }
    if (!AfxWinInit(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), 0)) return 1;
    try
    {
        if (argc == 3 && wcscmp(argv[1], L"--guard") == 0)
        {
            g_fixtureBase = fs::path(argv[2]);
            TestProcessGuard();
        }
        else
        {
            if (argc != 2) throw std::runtime_error("fixture path required");
            g_fixtureBase = fs::path(argv[1]);
            TestStrictJson();
            TestScopedIndex();
            TestUnsafeIds();
            Require(DecodeWorkspaceName(L"--C-Users-Demo-~4E2D~6587--") == L"C:\\Users\\Demo\\中文", "workspace drive decoding incorrect");
            TestSelectedDeletion();
            TestMissingDirectory();
            TestLockedSession();
            TestPartialDeletion();
            TestBackupFailure();
            TestMetadataFailure();
            TestStaleSelection();
            TestSnapshotPathIgnored();
            if (argc == 2) TestJunctions(fs::path(argv[1]));
        }
        std::cout << "PASS: " << g_assertions << " assertions\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; }
    catch (const CString&) { std::cerr << "FAIL: unexpected local data error\n"; }
    return 1;
}
