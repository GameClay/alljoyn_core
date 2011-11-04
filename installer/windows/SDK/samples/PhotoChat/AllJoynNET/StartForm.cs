using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Windows.Forms;

namespace AllJoynNET {
public partial class StartForm : Form {
    public StartForm()
    {
        InitializeComponent();
        _started = true;
    }

    public bool Started { get { return _started; } }
    private bool _started = false;

    private void btnStart_Click(object sender, EventArgs e)
    {
        this.Hide();
        /// INVOKE A Unit test
        ///
    }
}
}
