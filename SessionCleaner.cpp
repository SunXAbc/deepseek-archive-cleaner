// DeepSeek归档删除工具：Windows 原生界面，所有数据处理仅在本地进行。
#include <afxwin.h>
#include <afxcmn.h>
#include "SessionCleanerCore.h"

#ifndef SESSIONCLEANER_CORE_TEST
// 控件标识只用于本程序的窗口消息映射。
#define IDC_LIST        1001
#define IDC_BTN_ALL     1002
#define IDC_BTN_NONE    1003
#define IDC_BTN_INVERT  1004
#define IDC_BTN_DELETE  1005
#define IDC_BTN_REFRESH 1006
#define IDC_BTN_OPEN    1007

/** 列表列序号与界面创建顺序一致，不与磁盘数据结构绑定。 */
enum { COL_CHECK = 0, COL_TITLE, COL_ID, COL_CWD, COL_SIZE, COL_TIME, COL_STATUS, COL_COUNT };

// ---------------- 界面尺寸 ----------------

/**
 * 按应用清单声明的系统 DPI 缩放界面尺寸，使字体与控件保持相同比例。
 * @param logicalPixels 在 96 DPI 下的逻辑像素数，可为负值（字体高度）。
 * @return 当前系统 DPI 对应的物理像素数。
 */
static int UiPx(int logicalPixels)
{
    return MulDiv(logicalPixels, (int)GetDpiForSystem(), 96);
}

// ---------------- 主窗口 ----------------

/** 主窗口负责界面布局和现有会话操作，绘制调整不改变扫描与删除规则。 */
class CMainFrame : public CFrameWnd
{
public:
    /** 创建空窗口对象，控件与字体在 OnCreate 中分配。 */
    CMainFrame() = default;

protected:
    // 主窗口持有的会话表格。
    CListCtrl m_list;
    // 1 物理像素宽的空图标列表，将数据行高度固定到约 28 个逻辑像素。
    CImageList m_rowHeight;
    // 全选按钮，生命周期跟随主窗口。
    CButton m_btnAll;
    // 清空勾选按钮，生命周期跟随主窗口。
    CButton m_btnNone;
    // 反选按钮，生命周期跟随主窗口。
    CButton m_btnInv;
    // 危险操作按钮，宽度预留勾选数量的显示空间。
    CButton m_btnDel;
    // 重新扫描按钮。
    CButton m_btnRef;
    // 打开会话数据目录按钮。
    CButton m_btnOpen;
    // 主标题文本控件。
    CStatic m_title;
    // 会话数量、大小与数据来源的单行统计。
    CStatic m_subtitle;
    // 删除前退出 DSH 的操作提示。
    CStatic m_hint;
    // 正文和按钮共用的 14 逻辑像素字体。
    CFont m_font;
    // 主标题使用的 20 逻辑像素半粗字体。
    CFont m_fontTitle;

    DECLARE_MESSAGE_MAP()

    afx_msg int  OnCreate(LPCREATESTRUCT lp);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* mmi);
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void OnSelectAll();
    afx_msg void OnSelectNone();
    afx_msg void OnInvert();
    afx_msg void OnRefresh();
    afx_msg void OnOpenDir();
    afx_msg void OnDelete();
    afx_msg void OnDblClk(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnItemChanged(NMHDR* pNMHDR, LRESULT* pResult);

public:
    void Reload();
    void LayoutChildren();
    int  CountChecked();
    void UpdateSelectionUI();
    std::vector<SessionItem> m_items;
};

BEGIN_MESSAGE_MAP(CMainFrame, CFrameWnd)
    ON_WM_CREATE()
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_WM_ERASEBKGND()
    ON_WM_CTLCOLOR()
    ON_NOTIFY(NM_DBLCLK, IDC_LIST, &CMainFrame::OnDblClk)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIST, &CMainFrame::OnItemChanged)
    ON_COMMAND(IDC_BTN_ALL, &CMainFrame::OnSelectAll)
    ON_COMMAND(IDC_BTN_NONE, &CMainFrame::OnSelectNone)
    ON_COMMAND(IDC_BTN_INVERT, &CMainFrame::OnInvert)
    ON_COMMAND(IDC_BTN_REFRESH, &CMainFrame::OnRefresh)
    ON_COMMAND(IDC_BTN_OPEN, &CMainFrame::OnOpenDir)
    ON_COMMAND(IDC_BTN_DELETE, &CMainFrame::OnDelete)
END_MESSAGE_MAP()

/** @param lp 系统窗口创建参数，仅借用。@return 0 表示成功，-1 表示基类创建失败。 */
int CMainFrame::OnCreate(LPCREATESTRUCT lp)
{
    if (CFrameWnd::OnCreate(lp) == -1)
        return -1;

    // 字体、行高和按钮使用同一 DPI 比例，避免高分屏上只放大控件而文字仍过小。
    m_font.CreateFontW(-UiPx(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    m_fontTitle.CreateFontW(-UiPx(20), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    // 控件创建时的临时物理像素区域，创建完成后由布局函数统一定位。
    CRect rc(0, 0, 10, 10);
    // 列表使用 Windows 原生凹边框、选中态和表头，不接管系统绘制。
    m_list.CreateEx(WS_EX_CLIENTEDGE, WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                  LVS_SHOWSELALWAYS | WS_CLIPSIBLINGS, rc, this, IDC_LIST);
    // 去掉 DOUBLEBUFFER：某些主题/远程会话下它和列宽动态调整一起用会留下残影
    m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                            LVS_EX_CHECKBOXES);
    m_list.SetFont(&m_font);

    // 原生列表会附加一像素行间隔，因此图标高减一以得到约 28 逻辑像素行高。
    m_rowHeight.Create(1, UiPx(28) - 1, ILC_COLOR32, 1, 1);
    m_list.SetImageList(&m_rowHeight, LVSIL_SMALL);

    m_list.InsertColumn(COL_CHECK, L"", LVCFMT_LEFT, UiPx(36));
    m_list.InsertColumn(COL_TITLE, L"标题", LVCFMT_LEFT, UiPx(300));
    m_list.InsertColumn(COL_ID, L"会话 ID", LVCFMT_LEFT, UiPx(150));
    m_list.InsertColumn(COL_CWD, L"工作目录", LVCFMT_LEFT, UiPx(240));
    m_list.InsertColumn(COL_SIZE, L"占用", LVCFMT_RIGHT, UiPx(88));
    m_list.InsertColumn(COL_TIME, L"最后修改", LVCFMT_LEFT, UiPx(144));
    m_list.InsertColumn(COL_STATUS, L"状态", LVCFMT_LEFT, UiPx(84));

    /** 单个按钮的创建信息，只在窗口初始化期间借用按钮指针和固定文案。 */
    struct ButtonSpec
    {
        // 主窗口持有的按钮，不转移所有权。
        CButton* button;
        // 按钮初始文案，指向静态字符串。
        const wchar_t* text;
        // 用于消息映射的控件标识。
        UINT id;
    };
    // 将按钮创建信息集中排列，所有按钮使用同一正文尺寸。
    const ButtonSpec btns[] = {
        { &m_btnAll,  L"全选",     IDC_BTN_ALL },
        { &m_btnNone, L"全不选",   IDC_BTN_NONE },
        { &m_btnInv,  L"反选",     IDC_BTN_INVERT },
        { &m_btnRef,  L"刷新",     IDC_BTN_REFRESH },
        { &m_btnOpen, L"打开目录", IDC_BTN_OPEN },
        { &m_btnDel,  L"删除选中", IDC_BTN_DELETE },
    };
    // 为每个按钮增加键盘聚焦能力；点击通知仍沿用既有消息映射。
    for (const ButtonSpec& spec : btns)
    {
        spec.button->Create(spec.text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            rc, this, spec.id);
        spec.button->SetFont(&m_font);
    }

    // 标题区：大标题 + 统计副标题
    m_title.Create(L"DeepSeek归档删除工具", WS_CHILD | WS_VISIBLE | SS_LEFT, rc, this, 1010);
    m_title.SetFont(&m_fontTitle);

    m_subtitle.Create(L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, rc, this, 1011);
    m_subtitle.SetFont(&m_font);

    m_hint.Create(L"删除前请先退出 DSH", WS_CHILD | WS_VISIBLE | SS_RIGHT, rc, this, 1012);
    m_hint.SetFont(&m_font);

    Reload();
    return 0;
}

/** @param mmi 接收最小跟踪尺寸的系统结构，尺寸单位为物理像素；无返回值。 */
void CMainFrame::OnGetMinMaxInfo(MINMAXINFO* mmi)
{
    CFrameWnd::OnGetMinMaxInfo(mmi);
    mmi->ptMinTrackSize.x = UiPx(820);
    mmi->ptMinTrackSize.y = UiPx(500);
}

/** @param pDC 借用的窗口背景绘制上下文。@return TRUE，表示背景已完成擦除。 */
BOOL CMainFrame::OnEraseBkgnd(CDC* pDC)
{
    // 当前客户区域，单位为物理像素。
    CRect rc;
    GetClientRect(&rc);
    pDC->FillSolidRect(&rc, GetSysColor(COLOR_BTNFACE));
    return TRUE;
}

/**
 * 静态文字遵循 Windows 系统文字色和按钮面板底色，包括系统高对比主题。
 * @param pDC 借用的控件绘制上下文。
 * @param pWnd 正在绘制的控件，不转移所有权。
 * @param nCtlColor 系统控件类别。
 * @return 系统持有的背景画刷，不由调用者释放；非静态控件返回基类画刷。
 */
HBRUSH CMainFrame::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    if (nCtlColor == CTLCOLOR_STATIC)
    {
        pDC->SetBkMode(TRANSPARENT);
        pDC->SetTextColor(GetSysColor(COLOR_BTNTEXT));
        return GetSysColorBrush(COLOR_BTNFACE);
    }
    return CFrameWnd::OnCtlColor(pDC, pWnd, nCtlColor);
}

/** @param nType 系统尺寸变化类型。@param cx 客户区宽度，物理像素。@param cy 客户区高度，物理像素；无返回值。 */
void CMainFrame::OnSize(UINT nType, int cx, int cy)
{
    CFrameWnd::OnSize(nType, cx, cy);
    LayoutChildren();
}

/** 根据当前客户区重新分配控件和列宽；尺寸均先从逻辑像素转换，无参数和返回值。 */
void CMainFrame::LayoutChildren()
{
    if (!m_list.GetSafeHwnd()) return;

    // 当前窗口可用的物理像素区域，不包含标题栏和边框。
    CRect rcClient;
    GetClientRect(&rcClient);

    // 四边统一留白，给标题、操作和表格相同的左边界。
    const int pad = UiPx(20);
    // 同组按钮的物理像素间距。
    const int gap = UiPx(8);
    // 按钮高度与 14 逻辑像素文字匹配。
    const int btnH = UiPx(34);
    // 全部子控件布局的物理像素边界。
    CRect inner(pad, pad, rcClient.right - pad, rcClient.bottom - pad);
    if (inner.Width() < 200 || inner.Height() < 160) return;

    // 顶部：大标题 + 副标题
    m_title.MoveWindow(inner.left, inner.top, inner.Width() - UiPx(240), UiPx(30));
    m_hint.MoveWindow(inner.right - UiPx(220), inner.top + UiPx(6), UiPx(220), UiPx(22));
    m_subtitle.MoveWindow(inner.left, inner.top + UiPx(36), inner.Width(), UiPx(22));

    // 操作行与统计保持 10 逻辑像素距离，视觉层次清楚且不挤占表格。
    int y = inner.top + UiPx(68);
    // 当前按钮的物理像素横坐标，逐个向右累加。
    int x = inner.left;
    // 按钮依次排列；打开目录有四个汉字，单独留足内边距。
    for (CButton* button : { &m_btnAll, &m_btnNone, &m_btnInv, &m_btnRef, &m_btnOpen })
    {
        // 当前按钮宽度，单位为物理像素。
        const int width = UiPx(button == &m_btnOpen ? 100 : 80);
        button->MoveWindow(x, y, width, btnH);
        x += width + gap;
    }

    // 危险操作右对齐，宽度为勾选数量预留空间，避免文案被挤掉。
    const int delW = UiPx(148);
    // 最小窗口下也与左侧按钮组保留独立间距。
    int delX = max(inner.right - delW, x + gap);
    m_btnDel.MoveWindow(delX, y, delW, btnH);

    // 列表区域
    // 表格与按钮留出 14 逻辑像素，避免表头贴住操作区。
    int listTop = y + btnH + UiPx(14);
    // 表格完整使用内边距以内的宽度，单位为物理像素。
    int listW = inner.Width();
    // 表格一直延伸至底部内边距，不再保留没有内容的页脚占位。
    int listH = max(UiPx(60), inner.bottom - listTop);
    m_list.MoveWindow(inner.left, listTop, listW, listH);

    // 列宽按可用宽度分配：占用/时间/状态固定，其余按比例
    // 只有宽度真的变了才 SetColumnWidth，避免每次 WM_SIZE 都触发重排
    // 始终为垂直滚动条预留系统宽度，空列表与满列表的列宽保持一致。
    const int fixed = UiPx(36 + 88 + 144 + 84) + GetSystemMetrics(SM_CXVSCROLL)
        + 2 * GetSystemMetrics(SM_CXEDGE);
    // 标题、ID、工作目录可分配的剩余物理像素宽度。
    const int rest = max(0, listW - fixed);
    // 标题占剩余宽度的 46%，ID 占 22%，路径使用余数以免整数取整越界。
    const int titleW = rest * 46 / 100;
    // ID 仅作辅助定位，不再过度挤占可读标题。
    const int idW = rest * 22 / 100;
    /** 列宽更新参数，仅用于本轮窗口布局。 */
    struct ColumnWidth
    {
        // 系统列表中的目标列索引。
        int column;
        // 该列新的物理像素宽度。
        int width;
    };
    // 所有列共享同一次宽度计算，不会在窄窗口下保留旧的宽列。
    const ColumnWidth cols[] = {
        { COL_CHECK, UiPx(36) },
        { COL_TITLE, titleW },
        { COL_ID, idW },
        { COL_CWD, rest - titleW - idW },
        { COL_SIZE, UiPx(88) },
        { COL_TIME, UiPx(144) },
        { COL_STATUS, UiPx(84) },
    };
    // 只更新真正变化的列，降低缩放重绘残影和闪烁。
    for (const ColumnWidth& spec : cols)
    {
        if (m_list.GetColumnWidth(spec.column) != spec.width)
            m_list.SetColumnWidth(spec.column, spec.width);
    }

    // 布局变化后强制整块重画，避免列宽调整留下文字残影
    m_list.RedrawWindow(nullptr, nullptr,
                        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

/** 重新读取本地归档；读取失败时显示错误并清空旧快照，不把错误伪装为零条会话。 */
void CMainFrame::Reload()
{
    m_list.SetRedraw(FALSE);
    m_list.DeleteAllItems();
    m_items.clear();
    // 仅在本轮刷新失败时显示错误，不保存到日志。
    CString scanError;
    try { m_items = ScanSessions(); }
    catch (const CString& error) { scanError = error; }

    // i 是当前快照的列表行号，与 m_items 一一对应。
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        // 会话记录由主窗口持有，本循环只借用。
        const SessionItem& it = m_items[i];
        // 标题缺失时用工作区目录名还原成真实路径兜底，保证每行都有可读信息
        CString fallback = DecodeWorkspaceName(it.workspace);
        // 优先展示缓存标题，缺失时使用解码后的工作区名称。
        CString title = it.title;
        if (title.IsEmpty()) title = fallback.IsEmpty() ? CString(L"(无标题)") : fallback;

        // 工作目录仅用于显示，不作为删除目标。
        CString cwd = it.cwd;
        if (cwd.IsEmpty()) cwd = fallback.IsEmpty() ? CString(L"-") : fallback;

        // 目录缺失由状态列解释，占用列保留短文本以适应窄窗口。
        CString sizeText = it.path.IsEmpty() ? CString(L"-") : FormatSize(it.bytes);

        m_list.InsertItem((int)i, L"");
        m_list.SetItemText((int)i, COL_TITLE, title);
        m_list.SetItemText((int)i, COL_ID, it.id);
        m_list.SetItemText((int)i, COL_CWD, cwd);
        m_list.SetItemText((int)i, COL_SIZE, sizeText);
        m_list.SetItemText((int)i, COL_TIME, FormatTime(it.mtime));
        // 状态列：目录还在=正常；索引里有但目录没了=目录缺失
        m_list.SetItemText((int)i, COL_STATUS, it.path.IsEmpty() ? L"目录缺失" : L"正常");
    }
    m_list.SetRedraw(TRUE);
    // 整块重画（含滚动区），比单 Invalidate 更彻底
    m_list.RedrawWindow(nullptr, nullptr,
                        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);

    // 当前展示快照的总占用，单位为字节。
    ULONGLONG total = 0;
    // it 表示当前参与合计的快照记录。
    for (auto& it : m_items) total += it.bytes;
    // 副标题同时承担空状态和可操作错误的说明。
    CString st;
    if (!scanError.IsEmpty()) st = L"读取失败：" + scanError;
    else if (m_items.empty()) st = L"没有归档会话。归档后点击“刷新”。";
    else st.Format(L"%d 个归档会话  ·  合计 %s  ·  %s",
                   (int)m_items.size(), (LPCWSTR)FormatSize(total), (LPCWSTR)GetDshRoot());
    m_subtitle.SetWindowText(st);
    LayoutChildren();
    UpdateSelectionUI();
}

// 统计当前勾选数量
int CMainFrame::CountChecked()
{
    int n = 0;
    for (int i = 0; i < m_list.GetItemCount(); ++i)
        if (m_list.GetCheck(i)) ++n;
    return n;
}

/** 同步勾选数量；无选中项时禁用删除按钮，避免空列表出现可执行的危险操作。 */
void CMainFrame::UpdateSelectionUI()
{
    // 当前已勾选的条目数，不等同于系统高亮行数。
    int n = CountChecked();
    // 原生按钮文案为选中数量预留布局宽度。
    CString t;
    if (n > 0) t.Format(L"删除选中 (%d)", n);
    else       t = L"删除选中";
    m_btnDel.SetWindowText(t);
    m_btnDel.EnableWindow(n > 0);
    m_btnDel.Invalidate(FALSE);
}

// 勾选/取消勾选都会走到这里
void CMainFrame::OnItemChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* p = (LPNMLISTVIEW)pNMHDR;
    // 只关心状态变化（勾选态在 uChanged 的 STATEIMAGEMASK 位）
    if ((p->uChanged & LVIF_STATE) && ((p->uOldState ^ p->uNewState) & LVIS_STATEIMAGEMASK))
        UpdateSelectionUI();
    *pResult = 0;
}

void CMainFrame::OnSelectAll()
{
    for (int i = 0; i < m_list.GetItemCount(); ++i) m_list.SetCheck(i, TRUE);
}

void CMainFrame::OnSelectNone()
{
    for (int i = 0; i < m_list.GetItemCount(); ++i) m_list.SetCheck(i, FALSE);
}

void CMainFrame::OnInvert()
{
    for (int i = 0; i < m_list.GetItemCount(); ++i)
        m_list.SetCheck(i, m_list.GetCheck(i) ? FALSE : TRUE);
}

void CMainFrame::OnRefresh()
{
    Reload();
}

/** 使用系统文件管理器打开已校验的本地会话目录；不允许索引引导到网络或链接位置。 */
void CMainFrame::OnOpenDir()
{
    try
    {
        // 目录由受检根路径拼接而成，不使用会话标题和 cwd。
        const fs::path directory = DataRoot() / L"sessions";
        if (!IsPlainLocalPath(directory)) throw CString(L"会话目录不安全，无法打开。");
        if ((INT_PTR)ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
            throw CString(L"无法打开会话目录，请检查目录是否存在。");
    }
    catch (const CString& error) { AfxMessageBox(error, MB_ICONWARNING); }
}

void CMainFrame::OnDblClk(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* pItem = (LPNMITEMACTIVATE)pNMHDR;
    if (pItem->iItem >= 0)
        m_list.SetCheck(pItem->iItem, m_list.GetCheck(pItem->iItem) ? FALSE : TRUE);
    *pResult = 0;
}

/** 用户确认后执行本地清理；底层失败必须明确报告，随后重新读取实际状态。 */
void CMainFrame::OnDelete()
{
    // 固定本次操作的选择快照，防止重新加载列表时改变目标集合。
    std::vector<SessionItem> sel;
    // 确认框中的已知占用，单位为字节，不含缺失目录。
    ULONGLONG total = 0;
    // i 是待检查的列表行号，与当前快照记录一一对应。
    for (int i = 0; i < m_list.GetItemCount(); ++i)
    {
        if (m_list.GetCheck(i))
        {
            sel.push_back(m_items[i]);
            total += m_items[i].bytes;
        }
    }
    if (sel.empty())
    {
        AfxMessageBox(L"没有勾选任何会话。", MB_ICONINFORMATION);
        return;
    }
    if (IsDshRunning())
    {
        AfxMessageBox(L"DeepSeek Harness 正在运行，会话文件被占用。\n\n"
                      L"请先完全退出 DSH，再点「刷新」和「删除选中」。", MB_ICONWARNING);
        return;
    }

    // 永久删除前仅在本机显示数量和大小，不生成日志。
    CString msg;
    msg.Format(L"将永久删除 %d 个归档会话，共 %s。\n\n删除后不可恢复，确定继续？",
               (int)sel.size(), (LPCWSTR)FormatSize(total));
    if (AfxMessageBox(msg, MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;

    // 已提交成功的归档条目数量，包含缺失目录的残留索引。
    int okCount = 0;
    // 目录删除失败且已保留元数据的条目数量。
    int failCount = 0;
    // 执行期间禁用按钮，避免重入；扫描之后按勾选状态恢复。
    m_btnDel.EnableWindow(FALSE);
    try { DoDelete(sel, okCount, failCount); }
    catch (const CString& error)
    {
        AfxMessageBox(error, MB_ICONERROR);
        Reload();
        return;
    }

    // 完成信息区分成功清理与保留元数据的失败项。
    CString done;
    done.Format(L"已清理 %d 个归档条目，失败 %d 个。\n失败条目的索引与缓存已保留。", okCount, failCount);
    AfxMessageBox(done, failCount ? MB_ICONWARNING : MB_ICONINFORMATION);
    Reload();
}

// ---------------- 应用对象 ----------------

/** 应用入口解析既有命令行并创建主窗口；管理员权限由嵌入清单在入口执行前申请。 */
class CSessionCleanerApp : public CWinApp
{
private:
    // 无界面模式的退出码，不把会话路径或标题写入文件。
    int m_exitCode = 0;

public:
    /** @return 命令行操作的退出码；图形界面沿用 MFC 的退出结果。 */
    int ExitInstance() override
    {
        // 基类清理必须执行，然后再返回本次命令行结果。
        const int result = CWinApp::ExitInstance();
        return m_exitCode ? m_exitCode : result;
    }

    /** 无参数。@return TRUE 表示进入窗口消息循环，FALSE 表示命令行操作完成或窗口创建失败。 */
    BOOL InitInstance() override
    {
        // 无界面删除必须同时提供显式根目录和 -yes，避免误删默认用户目录。
        bool autoDelete = false;
        // 无界面模式的显式确认，仅影响当前调用。
        bool confirmed = false;
        // 参数错误阻止创建窗口或执行任何删除。
        bool invalidArguments = false;
        // Windows 分配的命令行参数数量。
        int argc = 0;
        // 参数数组由系统分配，解析完毕后通过 LocalFree 释放。
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        // 按原有顺序解析根目录覆盖和无界面模式。
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-root") == 0 && i + 1 < argc) g_rootOverride = argv[++i];
            else if (_wcsicmp(argv[i], L"-autodelete") == 0) autoDelete = true;
            else if (_wcsicmp(argv[i], L"-yes") == 0) confirmed = true;
            else invalidArguments = true;
        }
        if (argv) LocalFree(argv);
        if (invalidArguments || (autoDelete && (!confirmed || g_rootOverride.IsEmpty())))
        {
            m_exitCode = 2;
            if (!autoDelete) AfxMessageBox(L"参数错误。无界面清理需要同时指定 -root、-autodelete 和 -yes。", MB_ICONWARNING);
            return FALSE;
        }

        if (autoDelete)
        {
            try
            {
                // 显式根目录中的归档快照，不读取默认目录作为替代。
                const auto items = ScanSessions();
                // 已成功提交的清理条目数。
                int ok = 0;
                // 已保留元数据的失败条目数。
                int fail = 0;
                DoDelete(items, ok, fail);
                m_exitCode = fail ? 1 : 0;
            }
            catch (const CString&) { m_exitCode = 1; }
            return FALSE;
        }

        // 创建成功后由 MFC 主窗口生命周期管理；创建失败时本函数释放。
        auto* pFrame = new CMainFrame();

        // 可用工作区为物理像素，不包含任务栏；默认尺寸给增大的文字保留阅读空间。
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        // 当前桌面工作区的物理像素宽度。
        int availW = wa.right - wa.left;
        // 当前桌面工作区的物理像素高度。
        int availH = wa.bottom - wa.top;
        // 默认宽度上限为 1120 逻辑像素，同时适配较小屏幕。
        int w = min(UiPx(1120), (int)(availW * 0.84));
        // 默认高度上限为 680 逻辑像素，表格保持主要内容占比。
        int h = min(UiPx(680), (int)(availH * 0.78));
        if (w < UiPx(820)) w = min(UiPx(820), availW);
        if (h < UiPx(500)) h = min(UiPx(500), availH);
        // 窗口水平居中，坐标单位为物理像素。
        int left = wa.left + (availW - w) / 2;
        // 窗口略靠工作区上方，为底部任务栏留出视觉余地。
        int top = wa.top + (availH - h) / 3;

        if (!pFrame->Create(nullptr, L"DeepSeek归档删除工具",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CRect(left, top, left + w, top + h)))
        {
            delete pFrame;
            return FALSE;
        }
        m_pMainWnd = pFrame;
        pFrame->ShowWindow(SW_SHOW);
        pFrame->UpdateWindow();
        return TRUE;
    }
};

// 进程唯一的 MFC 应用对象，由 Windows 入口和 MFC 框架管理。
CSessionCleanerApp theApp;




#endif // SESSIONCLEANER_CORE_TEST
