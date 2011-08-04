/*
 * Copyright 2010 - 2011, Qualcomm Innovation Center, Inc.
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
 *
 */
package org.alljoyn.bus.daemonservice;

import java.util.LinkedList;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Handler;
import android.os.Message;
import android.util.Log;

/**
 * This class is counter-part of the BTLiteTransport class in native code. It manages service advertisement/discovery and actual inter-device
 * end point communication.
 */
public class BTLiteTransport{

	private final static String TAG = "BTLiteTransport";
	private final static int MSG_DO_DISCOVERY = 1;
    private Context mContext;
    private BluetoothAdapter mBluetoothAdapter = null;
	private BTLiteController mBTController = null;    
	private BTConnectionMgr mBTConnectionMgr = null;
	private LinkedList<BTDevice> mBtDeviceList = new LinkedList<BTDevice>();
    private NameService mNS = new NameService(this); 
    private Handler mHandler  = new Handler() {
        @Override
        public void handleMessage(Message msg) {
            switch (msg.what) {
            case MSG_DO_DISCOVERY:
            	Log.d(TAG, "handleMessage MSG_DO_DISCOVERY");
                doDiscovery();
                break;
            }
        }
    };
    
    public BTLiteTransport(Context context){
    	mContext = context;
    	mBluetoothAdapter = BluetoothAdapter.getDefaultAdapter();
    	mBTController = new BTLiteController(this);    
    	mBTConnectionMgr = new BTConnectionMgr(this);
        // Register the BroadcastReceiver
        // IntentFilter filter = new IntentFilter(BluetoothDevice.ACTION_FOUND);
        // mContext.registerReceiver(mReceiver, filter); // Don't forget to unregister during onDestroy
        // Message msg = mHandler.obtainMessage(MSG_DO_DISCOVERY);
        // mHandler.sendMessageDelayed(msg, 500);

    }
    
 /*   private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
		@Override
		public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            // When discovery finds a device
            if (BluetoothDevice.ACTION_FOUND.equals(action)) {
                // Get the BluetoothDevice object from the Intent
                BluetoothDevice device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                
                boolean found = false;
                for(BTDevice dev : mBtDeviceList){
                	if(dev.getDeviceAddr() == device.getAddress()){
                		found = true;
                	}
                }
                if(!found){
                	mBtDeviceList.add(new BTDevice(device.getAddress(), device.getName()));
                }
                Log.d(TAG, "Found Device: Name " + device.getName() + " MAC " + device.getAddress() + " Total count = " + mBtDeviceList.size());
            }
		}
    };*/
    
    public BTLiteController getBTLiteController(){
    	return mBTController;
    }

    public BTConnectionMgr getBTConnectionMgr(){
    	return mBTConnectionMgr;
    }
    
    public NameService getNameSerice(){
    	return mNS;
    }
    
    /**
     * Ensure this device can be discovered by other bluetooth devices
     */
    public void ensureDiscoverable() {
    	Log.d(TAG, "Ensure Self Discoverable");
        if (mBluetoothAdapter.getScanMode() !=
            BluetoothAdapter.SCAN_MODE_CONNECTABLE_DISCOVERABLE) {
            Intent discoverableIntent = new Intent(BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE);
            discoverableIntent.putExtra(BluetoothAdapter.EXTRA_DISCOVERABLE_DURATION, 300);
            mContext.startActivity(discoverableIntent);
        }
    }
 
    private void doDiscovery() {
        Log.d(TAG, "doDiscovery()");
        // If we're already discovering, stop it
        if (mBluetoothAdapter.isDiscovering()) {
        	mBluetoothAdapter.cancelDiscovery();
        }

        mBluetoothAdapter.startDiscovery();
    }
    
    public LinkedList<BTDevice> getBTDeviceList(){
    	return mBtDeviceList;
    }
}