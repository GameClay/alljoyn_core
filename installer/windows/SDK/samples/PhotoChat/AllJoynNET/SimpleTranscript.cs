using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Windows.Forms;

namespace AllJoynNET {
public partial class SimpleTranscript : Form {
    public SimpleTranscript()
    {
        InitializeComponent();
    }

    public void AddText(string text)
    {
        txtTranscript.Text += text;
        txtTranscript.Select(txtTranscript.Text.Length - 2, 1);
        txtTranscript.ScrollToCaret();
    }
}
}
