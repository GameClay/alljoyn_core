/*
 * Copyright 2011, Qualcomm Innovation Center, Inc.
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
 */
//----------------------------------------------------------------------------------------------

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Windows.Forms;
using System.Runtime.InteropServices;

namespace PhotoChat {
/// <summary>
/// An unused Form at this time.
/// Will eventually allow editing of AllJoyn session parameters
/// </summary>
public partial class AllJoynSetup : Form {
    #region Constructor
    //---------------------------------------------------------------------
    /// <summary>
    /// Construct the
    /// </summary>
    /// <param name="owner">
    /// The parent form.
    /// </param>
    public AllJoynSetup(ChatForm owner)
    {
        _owner = owner;
        InitializeComponent();
        this.Hide();
        _alljoyn = new AllJoynChatComponant();
        InterfaceName = _alljoyn.InterfaceName;
        NamePrefix = _alljoyn.NamePrefix;
        ObjectPath = _alljoyn.ObjectPath;
    }
    #endregion
    //---------------------------------------------------------------------
    #region Properties - internal (can be accessed by owner Form)

    internal AllJoynChatComponant AllJoyn { get { return _alljoyn; } }
    internal string SessionName { get { return txtSession.Text; } }
    internal string MyHandle { get { return txtHandle.Text; } }
    internal bool IsNameOwner { get { return rbAdvertise.Checked; } }

    #endregion
    //---------------------------------------------------------------------
    #region private variables

    private AllJoynChatComponant _alljoyn = null;
    private ChatForm _owner = null;

    internal string InterfaceName = "";
    internal string NamePrefix = "";
    internal string ObjectPath = "";

    #endregion
    //---------------------------------------------------------------------
    #region protected methods

    protected override void OnShown(EventArgs e)
    {
        if (_owner.Connected) {
            btnOk.Text = "Disconnect";
            txtSession.Enabled = false;
            txtHandle.Enabled = false;
            rbAdvertise.Enabled = false;
            rbJoin.Enabled = false;

        } else   {
            btnOk.Text = "Connect";
            txtSession.Enabled = true;
            txtHandle.Enabled = true;
            rbAdvertise.Enabled = true;
            rbJoin.Enabled = true;
        }

        base.OnShown(e);
    }

    private void btnOk_Click(object sender, EventArgs e)
    {

    }

    private void btnCancel_Click(object sender, EventArgs e)
    {

    }
    #endregion
}
}

