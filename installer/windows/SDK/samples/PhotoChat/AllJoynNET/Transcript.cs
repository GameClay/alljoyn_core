using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Windows.Forms;

namespace AllJoynNET {
public partial class Transcript : Form {
    public Transcript()
    {
        InitializeComponent();
        _connectForm = new AllJoynConnectForm();
        _trancriptText = new StringBuilder("New Session " + DateTime.Now.ToString());
        _trancriptText.AppendLine();     // EOL
    }

    private AllJoynConnectForm _connectForm = null;
    private StringBuilder _trancriptText = null;

    private void button2_Click(object sender, EventArgs e)
    {
        Close();
    }

    private void btnAllJoyn_Click(object sender, EventArgs e)
    {
        if (!_callbacksInstalled) {
            setCallbacks();
        }
        _connectForm.ShowDialog(this);
        txtTranscript.Text = _trancriptText.ToString();
        if (_connectForm.IsConnected) {
            _allJoyn = _connectForm.AJBus;
            _session = _connectForm.AJSession;
        }
    }

    // callback delegates - interface exported to native code
    private ReceiverDelegate _receiveDelegate;
    private SubscriberDelegate _joinDelegate;

    // callback functions
    private void receiveOutput(string data, ref int sz, ref int informType)
    {
        string it = informType.ToString() + ":";
        _trancriptText.AppendLine(it + data);
    }

    private AllJoynSession _session = null;
    private AllJoynBus _allJoyn = null;

    private void sessionSubscriber(string data, ref int sz)
    {
        MessageBox.Show("SUBSCRIBED" + data);
        if (_session == null)
            _session = new AllJoynSession(_allJoyn);
        _session.NewParticipant(data);
    }

    private bool _callbacksInstalled = false;

    private void setCallbacks()
    {
        if (!_callbacksInstalled) {
            _receiveDelegate = new ReceiverDelegate(receiveOutput);
            _joinDelegate = new SubscriberDelegate(sessionSubscriber);
            GC.KeepAlive(_receiveDelegate);
            GC.KeepAlive(_joinDelegate);
            _connectForm.InstallDelegates(_receiveDelegate, _joinDelegate);
            _callbacksInstalled = true;
        }
    }
}
}
