using System;
using System.IO;
using System.Management;
using System.Threading;
using Microsoft.Win32;

namespace ElishBrightness
{
    static class Program
    {
        private static readonly string LogPath = @"C:\elish\brightness-sync.log";
        private static int _targetBrightness = -1;
        private static int _lastAppliedBrightness = -1;
        private static Timer _debounceTimer;
        private static readonly object _lock = new object();

        private static void Log(string msg)
        {
            try
            {
                string line = string.Format("{0:yyyy-MM-dd HH:mm:ss.fff} {1}\r\n", DateTime.Now, msg);
                File.AppendAllText(LogPath, line);
            }
            catch {}
        }

        private static void ApplyBrightness(object state)
        {
            int val;
            lock (_lock)
            {
                val = _targetBrightness;
                if (val < 0 || val == _lastAppliedBrightness) return;
                _lastAppliedBrightness = val;
            }

            try
            {
                Registry.SetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\elish", "Brightness", val, RegistryValueKind.DWord);
                Log(string.Format("Sync -> HKLM\\SOFTWARE\\elish\\Brightness = {0}%", val));
            }
            catch (Exception ex)
            {
                Log("Error setting registry: " + ex.Message);
            }
        }

        private static void QueueBrightness(int percent)
        {
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;

            lock (_lock)
            {
                _targetBrightness = percent;
                _debounceTimer.Change(60, Timeout.Infinite);
            }
        }

        private static bool SyncCurrent()
        {
            try
            {
                using (var searcher = new ManagementObjectSearcher(@"root\wmi", "SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active = True"))
                {
                    foreach (ManagementObject mo in searcher.Get())
                    {
                        int cur = Convert.ToInt32(mo["CurrentBrightness"]);
                        Log(string.Format("Initial sync from WmiMonitorBrightness: {0}%", cur));
                        lock (_lock)
                        {
                            _targetBrightness = cur;
                            _lastAppliedBrightness = cur;
                        }
                        Registry.SetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\elish", "Brightness", cur, RegistryValueKind.DWord);
                        return true;
                    }
                }
            }
            catch (Exception ex)
            {
                Log("Initial sync waiting: " + ex.Message);
            }
            return false;
        }

        [MTAThread]
        static void Main(string[] args)
        {
            Log("=== ElishBrightnessSync started (auto-retry) ===");
            _debounceTimer = new Timer(ApplyBrightness, null, Timeout.Infinite, Timeout.Infinite);

            while (true)
            {
                while (!SyncCurrent())
                {
                    Thread.Sleep(2000);
                }

                try
                {
                    WqlEventQuery query = new WqlEventQuery("SELECT * FROM WmiMonitorBrightnessEvent");
                    using (var watcher = new ManagementEventWatcher(new ManagementScope(@"root\wmi"), query))
                    {
                        watcher.EventArrived += (s, e) =>
                        {
                            try
                            {
                                int brt = Convert.ToInt32(e.NewEvent["Brightness"]);
                                QueueBrightness(brt);
                            }
                            catch (Exception ex)
                            {
                                Log("Event handler error: " + ex.Message);
                            }
                        };

                        watcher.Start();
                        Log("Watcher listening on root\\wmi:WmiMonitorBrightnessEvent");

                        // Periodically verify WmiMonitorBrightness is still alive; reconnect if GPU/WMI reset
                        while (true)
                        {
                            Thread.Sleep(15000);
                            try
                            {
                                using (var s = new ManagementObjectSearcher(@"root\wmi", "SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active = True"))
                                {
                                    bool any = false;
                                    foreach (var _ in s.Get()) { any = true; break; }
                                    if (!any) throw new Exception("No active WmiMonitorBrightness");
                                }
                            }
                            catch (Exception ex)
                            {
                                Log("WMI check failed, reconnecting: " + ex.Message);
                                break;
                            }
                        }
                    }
                }
                catch (Exception ex)
                {
                    Log("Watcher exception, retrying in 3s: " + ex.Message);
                    Thread.Sleep(3000);
                }
            }
        }
    }
}
