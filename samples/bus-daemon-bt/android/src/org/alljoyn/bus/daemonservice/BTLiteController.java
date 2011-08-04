/*
 * Copyright 2009-2011, Qualcomm Innovation Center, Inc.
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

package org.alljoyn.bus.daemonservice;

import android.util.Log;

public class BTLiteController {
	private static final String TAG = "BTLiteController";
	private BTLiteTransport mBTTrans = null;
	
	public BTLiteController(BTLiteTransport trans){
		mBTTrans = trans;
	}
    /**
     * Put the device in discoverable state for a period of time
     */
	public void ensureDiscoverable(){
		Log.d(TAG, "ensureDiscoverable");
		mBTTrans.ensureDiscoverable();
	}
    	
	/**
	 * Notify the advertised name
	 */
	public void advertiseName(String name) {
		mBTTrans.getNameSerice().advertiseName(name);
	}
    	
	/**
	 * Remove an advertised name
	 */
	public void removeAvertizedName(String name) {
		mBTTrans.getNameSerice().unadvertiseName(name);
	}  
    	
	/**
	 * Wait for other nodes to discover advertise name
	 */
	public void startListen() {
		Log.d(TAG, "startListen");
	};
        
	/**
	 * Connect to discovered nodes and inquire about the advertised name
	 */
	public void startDiscovery(String namePrefix) {
		Log.d(TAG, "startDiscovery");
		mBTTrans.getNameSerice().locate(namePrefix);
   	}
    	   	
	public void stopDiscovery(String namePrefix) {
		Log.d(TAG, "stopDiscovery");
   	}
	
	/**
	 * Connect to a remote endpoint
	 */
	public String connect(String spec) {
		Log.d(TAG, "connect spec = " + spec);
		return mBTTrans.getBTConnectionMgr().connect(spec);
	}
    	
	/**
	 * Disconnect from a remote endpoint
	 */
	public int disConnect(String spec) {
		Log.d(TAG, "disConnect spec = " + spec);
		return 0;
	}

	/**
	 * Notify the endpoint to exit for resource release
	 */
	public void endpointExit(String uniqueID) {
		Log.d(TAG, "endpointExit uniqueID = " + uniqueID);
		mBTTrans.getBTConnectionMgr().endpointExit(uniqueID);
	}
	
	public native String getGlobalGUID();
	
	public native void foundName(String wkn, String guid, String addr, String port); 
	public native void accepted(String uniqueID);
    private long handle = 0; 
}
