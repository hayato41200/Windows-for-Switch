// SwitchClock: tray app for switchcpu.sys (Nintendo Switch CPU clock profiles).
//
// Talks to \\.\SwitchCpu (see SwitchCpuClock\src\switchcpu_ioctl.h). The icon
// shows the current clock in GHz; the menu selects a profile. The last
// non-experimental profile is put back when the app starts (HKCU), so an
// unstable experimental profile never comes back by itself after a reboot.
// Built with the .NET Framework 4 csc (C# 5); on Windows 10 ARM64 it runs as x86.
using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Text;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

static class SwCpu
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, int inLen, byte[] outBuf, int outLen, out int returned, IntPtr ov);

    const uint GET_STATUS = 0x00226000, GET_PROFILES = 0x00226004, SET_PROFILE = 0x0022A008;
    public const int FlagExperimental = 1, FlagDefault = 2;

    static byte[] Call(uint code, byte[] input, int outLen)
    {
        using (SafeFileHandle h = CreateFile(@"\\.\SwitchCpu", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero))
        {
            if (h.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            byte[] output = new byte[outLen];
            int returned;
            if (!DeviceIoControl(h, code, input, input == null ? 0 : input.Length, output, outLen, out returned, IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return output;
        }
    }

    public class Status
    {
        public int Khz, Mv, Active, Requested, TempSoC, TempBoard;
        public bool Throttled, TempValid;
    }

    public class Profile
    {
        public int Khz, Mv, Flags;
    }

    public static Status GetStatus()
    {
        byte[] b = Call(GET_STATUS, null, 48);
        Status s = new Status();
        s.Khz = BitConverter.ToInt32(b, 4);
        s.Mv = BitConverter.ToInt32(b, 8);
        s.Active = BitConverter.ToInt32(b, 12);
        s.Requested = BitConverter.ToInt32(b, 16);
        s.Throttled = BitConverter.ToInt32(b, 20) != 0;
        s.TempSoC = BitConverter.ToInt32(b, 24);
        s.TempBoard = BitConverter.ToInt32(b, 28);
        s.TempValid = BitConverter.ToInt32(b, 32) != 0;
        return s;
    }

    public static Profile[] GetProfiles()
    {
        byte[] b = Call(GET_PROFILES, null, 4 + 8 * 12);
        int count = Math.Min(BitConverter.ToInt32(b, 0), 8);
        Profile[] list = new Profile[count];
        for (int i = 0; i < count; i++)
        {
            list[i] = new Profile();
            list[i].Khz = BitConverter.ToInt32(b, 4 + i * 12);
            list[i].Mv = BitConverter.ToInt32(b, 8 + i * 12);
            list[i].Flags = BitConverter.ToInt32(b, 12 + i * 12);
        }
        return list;
    }

    public static void SetProfile(int index)
    {
        Call(SET_PROFILE, BitConverter.GetBytes(index), 0);
    }
}

// switchfan.sys v2 backlight (\\.\SwitchBacklight): brightness 8..255.
static class SwBacklight
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, int inLen, byte[] outBuf, int outLen, out int returned, IntPtr ov);

    const uint GET = 0x00226040, SET = 0x0022A044;

    static byte[] Call(uint code, byte[] input, int outLen)
    {
        using (SafeFileHandle h = CreateFile(@"\\.\SwitchBacklight", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero))
        {
            if (h.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            byte[] output = new byte[outLen];
            int returned;
            if (!DeviceIoControl(h, code, input, input == null ? 0 : input.Length, output, outLen, out returned, IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return output;
        }
    }

    // Returns the user brightness (8..255).
    public static int Get() { return BitConverter.ToInt32(Call(GET, null, 16), 4); }
    public static void Set(int level) { Call(SET, BitConverter.GetBytes(level), 0); }
}

class ClockTray : ApplicationContext
{
    const string SettingsKey = @"Software\SwitchWOA\SwitchClock";
    const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    const string RunName = "SwitchClock";
    static readonly string[] Names = { "省電力", "標準", "高め", "高性能", "最大寄り", "最大", "定格最大" };

    readonly NotifyIcon tray = new NotifyIcon();
    readonly ContextMenuStrip menu = new ContextMenuStrip();
    readonly ToolStripMenuItem header = new ToolStripMenuItem();
    readonly ToolStripMenuItem startup = new ToolStripMenuItem("ログイン時に起動");
    readonly ToolStripMenuItem brightness = new ToolStripMenuItem("画面の明るさ");
    static readonly int[] BrightnessSteps = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
    readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer();
    ToolStripMenuItem[] profileItems = new ToolStripMenuItem[0];
    SwCpu.Profile[] profiles;
    Icon currentIcon;
    Font boldFont;          // the running profile is bold, the requested one checked
    string iconKey = "";
    bool restored;

    public ClockTray()
    {
        header.Enabled = false;
        startup.Click += delegate { SetStartup(!startup.Checked); };
        menu.Opening += delegate { Refresh(); RefreshBrightness(); };
        tray.ContextMenuStrip = menu;
        tray.MouseUp += delegate(object sender, MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left)
            {
                // NotifyIcon only opens the menu on a right click; reuse its private ShowContextMenu.
                typeof(NotifyIcon).GetMethod("ShowContextMenu",
                    System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).Invoke(tray, null);
            }
        };
        BuildMenu();
        tray.Visible = true;
        timer.Interval = 2000;
        timer.Tick += delegate { Refresh(); };
        timer.Start();
        Refresh();
    }

    string ProfileLabel(int i)
    {
        SwCpu.Profile p = profiles[i];
        string name = i < Names.Length ? Names[i] : "プロファイル " + i;
        string label = string.Format("{0}  {1:0.0} MHz / {2} mV", name, p.Khz / 1000.0, p.Mv);
        if ((p.Flags & SwCpu.FlagDefault) != 0) label += "（起動時）";
        if ((p.Flags & SwCpu.FlagExperimental) != 0) label += "（試験的）";
        return label;
    }

    void BuildMenu()
    {
        menu.Items.Clear();
        menu.Items.Add(header);
        menu.Items.Add(new ToolStripSeparator());
        try { profiles = SwCpu.GetProfiles(); }
        catch (Win32Exception) { profiles = null; }
        if (profiles != null)
        {
            profileItems = new ToolStripMenuItem[profiles.Length];
            for (int i = 0; i < profiles.Length; i++)
            {
                int index = i;
                profileItems[i] = new ToolStripMenuItem(ProfileLabel(i));
                profileItems[i].Click += delegate { Choose(index); };
                menu.Items.Add(profileItems[i]);
            }
            menu.Items.Add(new ToolStripSeparator());
        }
        brightness.DropDownItems.Clear();
        foreach (int percent in BrightnessSteps)
        {
            int p = percent;
            ToolStripMenuItem item = new ToolStripMenuItem(p + "%");
            item.Tag = p;
            item.Click += delegate { SetBrightness(p); };
            brightness.DropDownItems.Add(item);
        }
        menu.Items.Add(brightness);
        menu.Items.Add(new ToolStripSeparator());
        startup.Checked = IsStartup();
        menu.Items.Add(startup);
        menu.Items.Add(new ToolStripMenuItem("終了", null, delegate { ExitThread(); }));
    }

    void Choose(int index)
    {
        SwCpu.Profile p = profiles[index];
        if ((p.Flags & SwCpu.FlagExperimental) != 0)
        {
            string text = string.Format(
                "{0} は試験的なプロファイルです（{1:0.0} MHz / {2} mV）。\n\n" +
                "固まったら、電源ボタンの長押しで電源を切ってください。再起動すると標準に戻ります。\n" +
                "このプロファイルは、次にログインしたときには自動で戻しません。\n\n切り替えますか？",
                Names[Math.Min(index, Names.Length - 1)], p.Khz / 1000.0, p.Mv);
            if (MessageBox.Show(text, "SwitchClock", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK)
                return;
        }
        try
        {
            SwCpu.SetProfile(index);
            if ((p.Flags & SwCpu.FlagExperimental) == 0)
                using (RegistryKey k = Registry.CurrentUser.CreateSubKey(SettingsKey)) k.SetValue("LastProfile", index);
        }
        catch (Win32Exception e)
        {
            MessageBox.Show("切り替えに失敗しました: " + e.Message, "SwitchClock", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        Refresh();
    }

    // Percent -> PWM duty; the driver keeps it (and restores it after a reboot).
    static int PercentToLevel(int percent) { return Math.Max(8, percent * 255 / 100); }

    void SetBrightness(int percent)
    {
        try { SwBacklight.Set(PercentToLevel(percent)); }
        catch (Win32Exception e)
        {
            MessageBox.Show("明るさを変えられませんでした: " + e.Message, "SwitchClock", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        RefreshBrightness();
    }

    void RefreshBrightness()
    {
        int level;
        try { level = SwBacklight.Get(); }
        catch (Win32Exception) { brightness.Visible = false; return; }
        brightness.Visible = true;
        int nearest = 0;
        foreach (ToolStripMenuItem item in brightness.DropDownItems)
        {
            int p = (int)item.Tag;
            if (Math.Abs(PercentToLevel(p) - level) < Math.Abs(PercentToLevel(nearest) - level)) nearest = p;
        }
        foreach (ToolStripMenuItem item in brightness.DropDownItems) item.Checked = ((int)item.Tag == nearest);
        brightness.Text = string.Format("画面の明るさ（{0}%）", level * 100 / 255);
    }

    void RestoreLastProfile()
    {
        restored = true;
        object v;
        using (RegistryKey k = Registry.CurrentUser.OpenSubKey(SettingsKey)) v = k == null ? null : k.GetValue("LastProfile");
        if (!(v is int) || profiles == null) return;
        int index = (int)v;
        if (index < 0 || index >= profiles.Length || (profiles[index].Flags & SwCpu.FlagExperimental) != 0) return;
        try { SwCpu.SetProfile(index); } catch (Win32Exception) { }
    }

    void Refresh()
    {
        SwCpu.Status s;
        try
        {
            if (profiles == null) BuildMenu();
            if (profiles != null && !restored) RestoreLastProfile();
            s = SwCpu.GetStatus();
        }
        catch (Win32Exception e)
        {
            SetIcon("?", Color.Gray);
            SetTip("SwitchClock: ドライバーに接続できません (" + e.NativeErrorCode + ")");
            header.Text = "ドライバーに接続できません";
            return;
        }
        string temp = s.TempValid ? string.Format("SoC {0}℃", s.TempSoC) : "温度不明";
        string tip = string.Format("{0:0.0} MHz / {1} mV / {2}", s.Khz / 1000.0, s.Mv, temp);
        if (s.Throttled) tip += " / 温度で制限中";
        SetTip(tip);
        header.Text = s.Throttled ? tip.Replace(" / 温度で制限中", "") + "（温度で標準に制限中）" : tip;
        if (boldFont == null) boldFont = new Font(menu.Font, FontStyle.Bold);
        for (int i = 0; i < profileItems.Length; i++)
        {
            profileItems[i].Checked = (i == s.Requested);
            profileItems[i].Font = (i == s.Active) ? boldFont : menu.Font;
        }
        bool experimental = s.Active >= 0 && profiles != null && s.Active < profiles.Length &&
                            (profiles[s.Active].Flags & SwCpu.FlagExperimental) != 0;
        Color color = s.Throttled ? Color.OrangeRed : experimental ? Color.Gold : Color.LightGreen;
        SetIcon((s.Khz / 1000000.0).ToString("0.0"), color);
    }

    void SetTip(string text)
    {
        tray.Text = text.Length > 63 ? text.Substring(0, 63) : text;
    }

    [DllImport("user32.dll")]
    static extern bool DestroyIcon(IntPtr handle);

    void SetIcon(string text, Color color)
    {
        string key = text + color.ToArgb();
        if (key == iconKey) return;
        iconKey = key;
        Size size = SystemInformation.SmallIconSize;
        using (Bitmap bmp = new Bitmap(size.Width, size.Height))
        using (Graphics g = Graphics.FromImage(bmp))
        using (Font font = new Font("Segoe UI", size.Height * 0.55f, FontStyle.Bold, GraphicsUnit.Pixel))
        using (SolidBrush back = new SolidBrush(Color.FromArgb(40, 40, 40)))
        using (SolidBrush fore = new SolidBrush(color))
        {
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.FillRectangle(back, 0, 0, size.Width, size.Height);
            StringFormat center = new StringFormat();
            center.Alignment = StringAlignment.Center;
            center.LineAlignment = StringAlignment.Center;
            g.DrawString(text, font, fore, new RectangleF(-2, 0, size.Width + 4, size.Height), center);
            IntPtr handle = bmp.GetHicon();
            Icon icon = (Icon)Icon.FromHandle(handle).Clone();
            DestroyIcon(handle);
            tray.Icon = icon;
            if (currentIcon != null) currentIcon.Dispose();
            currentIcon = icon;
        }
    }

    static bool IsStartup()
    {
        using (RegistryKey k = Registry.CurrentUser.OpenSubKey(RunKey))
            return k != null && k.GetValue(RunName) != null;
    }

    void SetStartup(bool on)
    {
        using (RegistryKey k = Registry.CurrentUser.CreateSubKey(RunKey))
        {
            if (on) k.SetValue(RunName, "\"" + Application.ExecutablePath + "\"");
            else k.DeleteValue(RunName, false);
        }
        startup.Checked = IsStartup();
    }

    protected override void ExitThreadCore()
    {
        timer.Stop();
        tray.Visible = false;
        tray.Dispose();
        base.ExitThreadCore();
    }

    [STAThread]
    static void Main()
    {
        bool created;
        using (Mutex single = new Mutex(true, @"Local\SwitchWOA.SwitchClock", out created))
        {
            if (!created) return;
            Application.EnableVisualStyles();
            Application.Run(new ClockTray());
        }
    }
}
