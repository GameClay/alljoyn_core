/**
 * @file
 * AlljoynDaemon.cs
 */

/******************************************************************************
 * Copyright 2010-2011, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 ******************************************************************************/

//#define ATTACH_DEBUGGER

// NOTE Uncomment the above #define if you need to debug the service but BEWARE
// - once this symbol is defined commenting out the symbol will NOT undefine it.
// it must be removed from the .vcproj file (with notepad or another text editor) 

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Diagnostics;
using System.ServiceProcess;
using System.Text;
using System.IO;
using System.Threading;

using System.Runtime.InteropServices;

namespace AlljoynService
{
    public partial class AlljoynDaemon : ServiceBase
    {
        public AlljoynDaemon()
        {
            InitializeComponent();
        }

        private const string _matchMe = "logpath=";
        private static string _logPath = "";
        private static string _exeName = "";
        private static string _cl = "";

        protected override void OnStart(string[] args)
        {
#if DEBUG
#if ATTACH_DEBUGGER
            while (!Debugger.IsAttached)      // Waiting until debugger is attached
            {
                RequestAdditionalTime(1000);  // Prevents the service from timeout
                Thread.Sleep(1000);           // Gives you time to attach the debugger   
            }
            RequestAdditionalTime(20000);     // for Debugging the OnStart method,
            // increase as needed to prevent timeouts

#endif
#endif
            // a service only passes the exe name with this property
            _exeName = Environment.CommandLine;

            // get any other command line strings
            int argc = args.Length; 
            foreach (string s in args)
            {
                string a = s.ToLower();
                a = a.Trim();
                if (a.Contains(_matchMe))
                {
                    _logPath = getLogPath(a);
                    argc--; // throw away
                }
                else
                    _cl += " " + a;
            }
            if (_logPath.Length == 0)
            {
#if DEBUG
                Debug.WriteLine("Path name for logfile not found {0}", _cl);
#endif
                return;
            }
            // Directory methods do not like quotes
            if (_logPath.IndexOf('"') > 0)
            {
#if DEBUG
                Debug.WriteLine("Path name for logfile not found {0}", _cl);
#endif
                return;
            }

            if (!Directory.Exists(_logPath))
            {
                try
                {
                    Directory.CreateDirectory(_logPath);
                }
                catch
                {
                    Debug.WriteLine("Access denied {0}", _logPath);
                    return;
                }
            }

            _logPath += @"\" + generateLogName();
            ServiceDLL.SetLogFile(_logPath);

            _cl = "AlljoynService " + _cl; 
            ThreadStart myThreadDelegate = new ThreadStart(DoWork);
            Thread myThread = new Thread(myThreadDelegate);
            myThread.Start();
        }

        protected override void OnStop()
        {
        }


        public static void DoWork()
        {
            ServiceDLL.DaemonMain(_cl);
        }

        private static string generateLogName()
        {
            string prefix = "DaemonLog_";
            DateTime dt = DateTime.Now;
            string s = dt.ToString("u");
            s = s.Replace(' ', '_'); // change embedded space
            s = s.Replace(':', '_'); // and colons
            s = s.ToLower().Replace('z', '.'); // change trailing z to .
            s += "txt";
            prefix += s;
            return prefix;
        }

        private static string getLogPath(string s)
        {
            string pathname = "";
            if (!s.Contains(_matchMe))
            {
#if DEBUG
                Debug.WriteLine("ERROR: logpath=<path name> not found.");
#endif
                return "";
            }

            int start = s.IndexOf(_matchMe);
            string remains = s.Remove(start, _matchMe.Length);

            // watch out for quoted pathnames
            int quote = remains.IndexOf('"', start, remains.Length - start);
            if (-1 != quote)
            {
                // find matching quote
                int endquote = remains.IndexOf('"', quote + 1);
                pathname = remains.Substring(quote, (endquote - quote) + 1);
                return pathname;
            }

            int stop = remains.IndexOf(' ', start, remains.Length - start);
            if (stop == -1) // no spaces found
                pathname = remains.Substring(start, remains.Length - start);
            else
            {
                pathname = remains.Substring(start, stop - start);
            }
            return pathname;
        }
    }
    internal class ServiceDLL
    {
        [DllImport("DaemonLib.dll", EntryPoint = "SetLogFile", CharSet = CharSet.Unicode)]
        internal extern static void SetLogFile(String path);
        [DllImport("DaemonLib.dll", EntryPoint = "DaemonMain", CharSet = CharSet.Unicode)]
        internal extern static void DaemonMain(String cmd);
    }
}
