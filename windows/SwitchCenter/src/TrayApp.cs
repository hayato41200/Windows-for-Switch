// Notification-area icon and menu: CPU profile, screen brightness, start at
// login. Stage 1 of the control center; same behaviour as SwitchClock.exe,
// which it replaces (settings are shared: HKCU\Software\SwitchWOA\SwitchClock).
using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Text;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Microsoft.Win32;

namespace SwitchCenter
{
    sealed class TrayApp : ApplicationContext
    {
        const string SettingsKey = @"Software\SwitchWOA\SwitchClock";     // kept from SwitchClock: LastProfile
        const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
        const string RunName = "SwitchCenter";
        const string OldRunName = "SwitchClock";
        static readonly string[] Names = { "省電力", "標準", "高め", "高性能", "最大寄り", "最大", "定格最大" };
        static readonly int[] BrightnessSteps = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };

        readonly NotifyIcon tray = new NotifyIcon();
        readonly ContextMenuStrip menu = new ContextMenuStrip();
        readonly ToolStripMenuItem header = new ToolStripMenuItem { Enabled = false };
        readonly ToolStripMenuItem brightness = new ToolStripMenuItem("画面の明るさ");
        readonly ToolStripMenuItem startup = new ToolStripMenuItem("ログイン時に起動");
        readonly Timer timer = new Timer { Interval = 2000 };
        ToolStripMenuItem[] profileItems = Array.Empty<ToolStripMenuItem>();
        CpuClock.Profile[] profiles;
        Font boldFont;
        Icon currentIcon;
        string iconKey = "";
        bool restored;

        public TrayApp()
        {
            startup.Click += (_, _) => SetStartup(!startup.Checked);
            menu.Opening += (_, _) => { RefreshCpu(); RefreshBrightness(); };
            tray.ContextMenuStrip = menu;
            tray.MouseUp += (_, e) =>
            {
                // NotifyIcon opens the menu only on a right click; reuse its private ShowContextMenu.
                if (e.Button == MouseButtons.Left)
                    typeof(NotifyIcon).GetMethod("ShowContextMenu",
                        System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)?.Invoke(tray, null);
            };
            BuildMenu();
            tray.Visible = true;
            timer.Tick += (_, _) => RefreshCpu();
            timer.Start();
            RefreshCpu();
        }

        string ProfileLabel(CpuClock.Profile p)
        {
            string name = p.Index < Names.Length ? Names[p.Index] : "プロファイル " + p.Index;
            string label = $"{name}  {p.Khz / 1000.0:0.0} MHz / {p.Mv} mV";
            if (p.Default) label += "（起動時）";
            if (p.Experimental) label += "（試験的）";
            return label;
        }

        void BuildMenu()
        {
            menu.Items.Clear();
            menu.Items.Add(header);
            menu.Items.Add(new ToolStripSeparator());
            try { profiles = CpuClock.GetProfiles(); }
            catch (Win32Exception) { profiles = null; }
            if (profiles != null)
            {
                profileItems = new ToolStripMenuItem[profiles.Length];
                foreach (CpuClock.Profile p in profiles)
                {
                    var item = new ToolStripMenuItem(ProfileLabel(p));
                    int index = p.Index;
                    item.Click += (_, _) => Choose(index);
                    profileItems[p.Index] = item;
                    menu.Items.Add(item);
                }
                menu.Items.Add(new ToolStripSeparator());
            }
            brightness.DropDownItems.Clear();
            foreach (int percent in BrightnessSteps)
            {
                var item = new ToolStripMenuItem(percent + "%") { Tag = percent };
                item.Click += (_, _) => SetBrightness(percent);
                brightness.DropDownItems.Add(item);
            }
            menu.Items.Add(brightness);
            menu.Items.Add(new ToolStripSeparator());
            startup.Checked = IsStartup();
            menu.Items.Add(startup);
            menu.Items.Add(new ToolStripMenuItem("終了", null, (_, _) => ExitThread()));
        }

        void Choose(int index)
        {
            CpuClock.Profile p = profiles[index];
            if (p.Experimental)
            {
                string text = $"{Names[Math.Min(index, Names.Length - 1)]} は試験的なプロファイルです（{p.Khz / 1000.0:0.0} MHz / {p.Mv} mV）。\n\n" +
                              "固まったら、電源ボタンの長押しで電源を切ってください。再起動すると標準に戻ります。\n" +
                              "このプロファイルは、次にログインしたときには自動で戻しません。\n\n切り替えますか？";
                if (MessageBox.Show(text, "Switch コントロールセンター", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK)
                    return;
            }
            try
            {
                CpuClock.SetProfile(index);
                if (!p.Experimental)
                    using (RegistryKey k = Registry.CurrentUser.CreateSubKey(SettingsKey)) k.SetValue("LastProfile", index);
            }
            catch (Win32Exception e)
            {
                MessageBox.Show("切り替えに失敗しました: " + e.Message, "Switch コントロールセンター", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            RefreshCpu();
        }

        void RestoreLastProfile()
        {
            restored = true;
            object v;
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(SettingsKey)) v = k?.GetValue("LastProfile");
            if (v is not int index || profiles == null) return;
            if (index < 0 || index >= profiles.Length || profiles[index].Experimental) return;
            try { CpuClock.SetProfile(index); } catch (Win32Exception) { }
        }

        void SetBrightness(int percent)
        {
            try { Backlight.Set(Backlight.PercentToLevel(percent)); }
            catch (Win32Exception e)
            {
                MessageBox.Show("明るさを変えられませんでした: " + e.Message, "Switch コントロールセンター", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            RefreshBrightness();
        }

        void RefreshBrightness()
        {
            Backlight.Status b;
            try { b = Backlight.Get(); }
            catch (Win32Exception) { brightness.Visible = false; return; }
            brightness.Visible = true;
            int nearest = 0;
            foreach (ToolStripMenuItem item in brightness.DropDownItems)
            {
                int p = (int)item.Tag;
                if (Math.Abs(Backlight.PercentToLevel(p) - b.Level) < Math.Abs(Backlight.PercentToLevel(nearest) - b.Level)) nearest = p;
            }
            foreach (ToolStripMenuItem item in brightness.DropDownItems) item.Checked = (int)item.Tag == nearest;
            brightness.Text = $"画面の明るさ（{Backlight.LevelToPercent(b.Level)}%）";
        }

        void RefreshCpu()
        {
            CpuClock.Status s;
            try
            {
                if (profiles == null) BuildMenu();
                if (profiles != null && !restored) RestoreLastProfile();
                s = CpuClock.GetStatus();
            }
            catch (Win32Exception e)
            {
                SetIcon("?", Color.Gray);
                SetTip($"Switch コントロールセンター: ドライバーに接続できません ({e.NativeErrorCode})");
                header.Text = "CPU のドライバーに接続できません";
                return;
            }
            string temp = s.TempValid ? $"SoC {s.TempSoC}℃" : "温度不明";
            string status = $"{s.Khz / 1000.0:0.0} MHz / {s.Mv} mV / {temp}";
            SetTip(s.Throttled ? status + " / 温度で制限中" : status);
            header.Text = s.Throttled ? status + "（温度で標準に制限中）" : status;
            boldFont ??= new Font(menu.Font, FontStyle.Bold);
            for (int i = 0; i < profileItems.Length; i++)
            {
                profileItems[i].Checked = i == s.Requested;
                profileItems[i].Font = i == s.Active ? boldFont : menu.Font;
            }
            bool experimental = profiles != null && s.Active >= 0 && s.Active < profiles.Length && profiles[s.Active].Experimental;
            SetIcon((s.Khz / 1000000.0).ToString("0.0"), s.Throttled ? Color.OrangeRed : experimental ? Color.Gold : Color.LightGreen);
        }

        void SetTip(string text) => tray.Text = text.Length > 63 ? text.Substring(0, 63) : text;

        [DllImport("user32.dll")]
        static extern bool DestroyIcon(IntPtr handle);

        void SetIcon(string text, Color color)
        {
            string key = text + color.ToArgb();
            if (key == iconKey) return;
            iconKey = key;
            Size size = SystemInformation.SmallIconSize;
            using var bmp = new Bitmap(size.Width, size.Height);
            using Graphics g = Graphics.FromImage(bmp);
            using var font = new Font("Segoe UI", size.Height * 0.55f, FontStyle.Bold, GraphicsUnit.Pixel);
            using var back = new SolidBrush(Color.FromArgb(40, 40, 40));
            using var fore = new SolidBrush(color);
            using var center = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.FillRectangle(back, 0, 0, size.Width, size.Height);
            g.DrawString(text, font, fore, new RectangleF(-2, 0, size.Width + 4, size.Height), center);
            IntPtr handle = bmp.GetHicon();
            var icon = (Icon)Icon.FromHandle(handle).Clone();
            DestroyIcon(handle);
            tray.Icon = icon;
            currentIcon?.Dispose();
            currentIcon = icon;
        }

        static bool IsStartup()
        {
            using RegistryKey k = Registry.CurrentUser.OpenSubKey(RunKey);
            return k?.GetValue(RunName) != null;
        }

        void SetStartup(bool on)
        {
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(RunKey))
            {
                if (on)
                {
                    k.SetValue(RunName, "\"" + Application.ExecutablePath + "\"");
                    k.DeleteValue(OldRunName, false);          // SwitchClock.exe is replaced by this app
                }
                else
                {
                    k.DeleteValue(RunName, false);
                }
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
    }
}
