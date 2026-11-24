# On-screen semi-transparent Touch Gamepad Overlay for Windows ARM64 games (FlatOut 2, GTA, etc.)
# Uses WS_EX_NOACTIVATE | WS_EX_TOPMOST so touching buttons NEVER steals focus from the game,
# and sends Hardware Scan Codes via SendInput (works with DirectInput games!).
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$code = @"
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

public class TouchPadForm : Form {
    const int WS_EX_NOACTIVATE = 0x08000000;
    const int WS_EX_TOPMOST    = 0x00000008;
    const int WS_EX_TOOLWINDOW = 0x00000080;

    [StructLayout(LayoutKind.Sequential)]
    struct INPUT { public uint type; public InputUnion u; }
    [StructLayout(LayoutKind.Explicit)]
    struct InputUnion { [FieldOffset(0)] public KEYBDINPUT ki; [FieldOffset(0)] public MOUSEINPUT mi; }
    [StructLayout(LayoutKind.Sequential)]
    struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }

    [DllImport("user32.dll", SetLastError = true)]
    static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

    const uint INPUT_KEYBOARD = 1;
    const uint KEYEVENTF_EXTENDEDKEY = 0x0001;
    const uint KEYEVENTF_KEYUP = 0x0002;
    const uint KEYEVENTF_SCANCODE = 0x0008;

    static void SendScan(ushort scan, bool ext, bool up) {
        INPUT[] inp = new INPUT[1];
        inp[0].type = INPUT_KEYBOARD;
        inp[0].u.ki.wScan = scan;
        inp[0].u.ki.dwFlags = KEYEVENTF_SCANCODE | (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
        SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
    }

    protected override CreateParams CreateParams {
        get {
            CreateParams cp = base.CreateParams;
            cp.ExStyle |= WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
            return cp;
        }
    }
    protected override bool ShowWithoutActivation { get { return true; } }

    Button AddPadBtn(string text, int x, int y, int w, int h, ushort scan, bool ext, Color bg) {
        var b = new Button();
        b.Text = text;
        b.SetBounds(x, y, w, h);
        b.FlatStyle = FlatStyle.Flat;
        b.FlatAppearance.BorderColor = Color.White;
        b.FlatAppearance.BorderSize = 2;
        b.BackColor = bg;
        b.ForeColor = Color.White;
        b.Font = new Font("Segoe UI", 14, FontStyle.Bold);
        b.TabStop = false;
        b.MouseDown += (s, e) => { b.BackColor = Color.DodgerBlue; SendScan(scan, ext, false); };
        b.MouseUp   += (s, e) => { b.BackColor = bg; SendScan(scan, ext, true); };
        b.MouseLeave += (s, e) => { if (b.BackColor == Color.DodgerBlue) { b.BackColor = bg; SendScan(scan, ext, true); } };
        this.Controls.Add(b);
        return b;
    }

    public TouchPadForm() {
        var scr = Screen.PrimaryScreen.Bounds;
        this.FormBorderStyle = FormBorderStyle.None;
        this.StartPosition = FormStartPosition.Manual;
        this.Bounds = new Rectangle(0, scr.Height - 230, scr.Width, 220);
        this.BackColor = Color.Magenta;
        this.TransparencyKey = Color.Magenta;
        this.Opacity = 0.72;
        this.TopMost = true;

        int W = scr.Width;
        // Left side: Steering Left / Right + Reset (R) + ESC
        AddPadBtn("◀", 24,  70, 120, 110, 0x4B, true, Color.FromArgb(40,40,55));   // Left Arrow
        AddPadBtn("▶", 160, 70, 120, 110, 0x4D, true, Color.FromArgb(40,40,55));   // Right Arrow
        AddPadBtn("ESC", 24, 8, 85, 48, 0x01, false, Color.FromArgb(90,40,40));    // Esc
        AddPadBtn("R", 120, 8, 70, 48, 0x13, false, Color.FromArgb(70,60,30));     // R (Reset car)

        // Right side: Gas (Up), Brake (Down), Nitro (LCtrl), Handbrake (Space), Enter
        AddPadBtn("ГАЗ ▲", W - 150, 70, 126, 110, 0x48, true, Color.FromArgb(30,110,45));  // Up Arrow
        AddPadBtn("ТОРМ ▼", W - 290, 70, 126, 110, 0x50, true, Color.FromArgb(130,35,35)); // Down Arrow
        AddPadBtn("NITRO", W - 150, 8, 126, 48, 0x1D, false, Color.FromArgb(160,90,10));   // Left Ctrl
        AddPadBtn("SPACE", W - 290, 8, 126, 48, 0x39, false, Color.FromArgb(60,60,90));    // Space
        AddPadBtn("ENTER", W - 405, 8, 100, 48, 0x1C, false, Color.FromArgb(45,85,120));   // Enter

        // Close button top-center
        var closeBtn = new Button();
        closeBtn.Text = "✕ Геймпад";
        closeBtn.SetBounds(W / 2 - 65, 8, 130, 40);
        closeBtn.FlatStyle = FlatStyle.Flat;
        closeBtn.BackColor = Color.FromArgb(110, 30, 30);
        closeBtn.ForeColor = Color.White;
        closeBtn.Font = new Font("Segoe UI", 10, FontStyle.Bold);
        closeBtn.Click += (s, e) => this.Close();
        this.Controls.Add(closeBtn);
    }
}
"@
Add-Type -TypeDefinition $code -ReferencedAssemblies System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::Run((New-Object TouchPadForm))
