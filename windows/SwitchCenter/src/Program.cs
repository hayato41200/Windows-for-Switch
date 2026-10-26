// Switch control center (stage 1: notification-area icon). See
// Windows-Drivers\Design-ControlCenter-ja.md for the plan.
using System;
using System.Threading;
using System.Windows.Forms;

namespace SwitchCenter
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            using var single = new Mutex(true, @"Local\SwitchWOA.SwitchCenter", out bool created);
            if (!created) return;
            Application.EnableVisualStyles();
            Application.SetHighDpiMode(HighDpiMode.SystemAware);
            Application.Run(new TrayApp());
        }
    }
}
