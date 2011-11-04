using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows.Forms;
using System.Threading;

namespace AllJoynNET.NET {
    static class Program {
        /// <summary>
        /// The main entry point for the application.
        /// </summary>
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            StartForm start = new StartForm();
            Application.Run(start);
            MessageBox.Show("Done");
            start.Show();
//            Application.Run(new Transcript());
        }
    }
}
