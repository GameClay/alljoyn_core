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

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Field;
import java.util.Iterator;
import java.util.LinkedList;
import java.util.List;
import java.util.Set;
import java.util.UUID;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.util.Log;

/**
 * This module manages advertisement and discovery of Alljoyn well-known names. During name discovery, the device
 * should query the bonded devices one by one. Thus this requires paring first.
 *
 */
public class NameService {
    private static final String TAG = "NameService";

    private BluetoothAdapter mBluetoothAdapter = null;
    private BTLiteTransport mBTTrans = null;
    private List<String> mAdvertizedNameList = null;
    private static final UUID NAMESERIVCE_UUID = UUID.fromString("e9a24e10-b190-11e0-a00b-0800200c9a66");
    private static final String NAME_DISCOVERY = "discovery";
    private LinkedList<BTDevice> mQueryList = null;
    public final UUID mMyServiceUUID = UUID.randomUUID();
    
    private int mState = STATE_IDLE;    
    public final static int STATE_IDLE = 0;
    public final static int STATE_QUERYING = 1;

    
    public NameService(BTLiteTransport trans){
    	mBTTrans = trans;
    	mAdvertizedNameList = new LinkedList<String>();
    	mQueryList = new LinkedList<BTDevice>();
    	mBluetoothAdapter  = BluetoothAdapter.getDefaultAdapter();
    	new ListenThread().start();
    }

    /**
     * Service start to advertise a well-know name.
     * @param name
     * @return
     */
    public boolean advertiseName(String name){  
    	mAdvertizedNameList.add(name);
    	Set<BluetoothDevice> pairedDevices = mBluetoothAdapter.getBondedDevices();
    	if (pairedDevices.size() > 0) {
    		Iterator<BluetoothDevice> it = pairedDevices.iterator();
    		while (it.hasNext()) {
    			mQueryList.add(new BTDevice(BTDevice.ACTION_ADVERTISE, it.next().getAddress(), name));
    	    }
    		if(mState == STATE_IDLE){
    	    	queryNextDevice();
    		}
    	}
    	return true;
    }
    /**
     * Service stop to advertise a well-know name
     * @param name
     * @return
     */
    public boolean unadvertiseName(String name){
		return mAdvertizedNameList.remove(name);
    }

    /**
     * Well-known name Discovery with given name prefix
     * @param namePrefix
     * @return
     */
    public synchronized boolean locate(String namePrefix){
    	Set<BluetoothDevice> pairedDevices = mBluetoothAdapter.getBondedDevices();
    	if (pairedDevices.size() > 0) {
    		Iterator<BluetoothDevice> it = pairedDevices.iterator();
    		while (it.hasNext()) {
    			mQueryList.add(new BTDevice(BTDevice.ACTION_DISCOVER, it.next().getAddress(), namePrefix));
    	    }
    		if(mState == STATE_IDLE){
    	    	queryNextDevice();
    		}
    	}else{
    		Log.d(TAG, " No bounded Device ");
    		return false;
    	}
    	return true;
    }
 
    /**
     * Try query next device for the well-know name
     */
    public synchronized void queryNextDevice(){
    	Log.d(TAG, "Query next device for well-know name");
		BTDevice btEntry = null;
		if (mQueryList.isEmpty()) {
			mState = STATE_IDLE;
			return;
		}
		mState = STATE_QUERYING;
		btEntry = mQueryList.poll();
    	new ConnectThread(btEntry).start();
    }
   
    /**
     * Find names that match the prefix and concate them into a string
     * @param prefix
     * @return names that match
     */
    public String matchNamePrefix(String prefix){
    	if(mAdvertizedNameList.isEmpty()){
    		Log.d(TAG, "No well-known names advertised");
    	}
    	String ret = "";
    	for(int i = 0; i < mAdvertizedNameList.size(); i++){
    		//Log.i(TAG, "advertised Name " + i + ": " + mAdvertizedNameList.get(i));
    		if(mAdvertizedNameList.get(i).startsWith(prefix)){
    			ret += mAdvertizedNameList.get(i);
    			Log.d(TAG, "Matched name: " + mAdvertizedNameList.get(i));
    			if(i != (mAdvertizedNameList.size()-1)){
    				ret += ";";
    			}
    		}
    	}
    	return ret;
    }
   
    /**
     * Return the local service UUID.
     * @return
     */
    public UUID getMyServiceUUID(){
    	return mMyServiceUUID;
    }
    
    /**
     * Thread that initiates connection to a remote listening device.
     */
    private class ConnectThread extends Thread {
        private BluetoothSocket mSocket = null;
        private BTDevice mConnectInfo = null;
        private BluetoothDevice mDevice = null;
     
        public ConnectThread(BTDevice connectInfo) {
            mDevice = mBluetoothAdapter.getRemoteDevice(connectInfo.getDeviceAddr());
            mConnectInfo = connectInfo;
        }
     
        @Override
        public void run() {
            // Cancel discovery because it will slow down the connection
            Log.d(TAG, "ConnectThread started");
            if (mBluetoothAdapter.isDiscovering()) {
            	mBluetoothAdapter.cancelDiscovery();
            }
     
            try {
                // Connect the device through the socket. This will block until it succeeds or throws an exception
               	mSocket = mDevice.createRfcommSocketToServiceRecord(NAMESERIVCE_UUID);
                mSocket.connect();
            } catch (IOException e) {
                Log.e(TAG, "Connect fail !" + e.getMessage());
                try {
					mSocket.close();
				} catch (IOException e1) {}
				queryNextDevice();
    			return;
            }
            new ConnectedThread(mSocket, ConnectedThread.TYPE_CLIENT, mConnectInfo).start();
        }
    }
    
    /**
     * Thread that listens and accepts connections from remote devices for name query.
     */
    private class ListenThread extends Thread {
        private BluetoothServerSocket mmServerSocket;
     
        public ListenThread() {
            try {
            	mmServerSocket = mBluetoothAdapter.listenUsingRfcommWithServiceRecord(NAME_DISCOVERY, NAMESERIVCE_UUID);
            } catch (IOException e) {
            	Log.e(TAG, "IOException: " + e.getMessage());
            }
        }
     
        @Override
        public void run() {
            BluetoothSocket socket = null;
            Log.d(TAG, "ListenThread started");
            if (mBluetoothAdapter.isDiscovering()) {
            	mBluetoothAdapter.cancelDiscovery();
            }
            try {
            	while (true) {
            		socket = mmServerSocket.accept();
            		if (socket != null) {
            			ConnectedThread connectedThread = new ConnectedThread(socket, ConnectedThread.TYPE_SERVER, null);
            			connectedThread.start();
            		}
            	}
            } catch (IOException e) {
            	Log.e(TAG, "ListenThread accept error");
            }

            try {
				mmServerSocket.close();
			} catch (IOException e) { }
        }
    }
    
    /**
     * Thread that handles all incoming and outgoing transmissions over a connection.
     */
    private class ConnectedThread extends Thread {
        private final BluetoothSocket mmSocket;
        private final InputStream mmInStream;
        private final OutputStream mmOutStream;
        private BTDevice mConnectInfo = null;
        private int mThreadType = -1;
		public final static int TYPE_SERVER = 0;
		public final static int TYPE_CLIENT = 1;
			
        public ConnectedThread(BluetoothSocket socket, int threadType, BTDevice connectInfo) {
            Log.d(TAG, "Create ConnectedThread: " + threadType);
            mmSocket = socket;
            mThreadType = threadType;
            mConnectInfo = connectInfo;
            InputStream tmpIn = null;
            OutputStream tmpOut = null;

            // Get the BluetoothSocket input and output streams
            try {
                tmpIn = socket.getInputStream();
                tmpOut = socket.getOutputStream();
            } catch (IOException e) {
                Log.e(TAG, "temp sockets not created", e);
            }

            mmInStream = tmpIn;
            mmOutStream = tmpOut;
        }

        @Override
        public void run() {
            Log.i(TAG, "BEGIN mConnectedThread mThreadType = " + mThreadType);
            if(mThreadType == TYPE_CLIENT){
            	switch(mConnectInfo.getAction()){
            		case BTDevice.ACTION_ADVERTISE:
            			NameServiceMessage advRequest = new NameServiceMessage(NameServiceMessage.TYPE_ADVERTISE_NAME_REQUEST);
            			advRequest.fillAdvertiseRequest(mBTTrans.getBTLiteController().getGlobalGUID(), mMyServiceUUID.toString(), mConnectInfo.getName());
            			byte[] advRequestArray = Utils.serializeObject(advRequest);
            			write(advRequestArray);
            			break;
            		case BTDevice.ACTION_DISCOVER:            		
            			NameServiceMessage disRequest = new NameServiceMessage(NameServiceMessage.TYPE_DISCOVER_NAME_REQUEST);
            			disRequest.fillDiscoverRequest(mConnectInfo.getName());
						byte[] disRequestArray = Utils.serializeObject(disRequest);
						write(disRequestArray);
						break;
            		default:
            			Log.e(TAG, "Unknow Action Type = " + mThreadType);
            			return;
            	}
            }
			handleRead();
        }

		void handleRead() {
			int bufferSize = 4048;
            byte[] buffer = new byte[bufferSize];
            int bytesRead;
            try {
            	while (true) {	
                    bytesRead = mmInStream.read(buffer);
                    if(bytesRead >= bufferSize){
                    	Log.e(TAG, "Buffer is too small!");
                    }
                    NameServiceMessage message = (NameServiceMessage) Utils.deserializeObject(buffer);
                    
                    Log.i(TAG, "Received NameServiceMessage Type :" + message.getType());
                    switch(message.getType()){
                    	case NameServiceMessage.TYPE_ADVERTISE_NAME_REQUEST:
                            if(message.getWkn().length() > 0){
                            	String addr = mmSocket.getRemoteDevice().getAddress();
                            	NameService.this.mBTTrans.getBTConnectionMgr().insertServiceRercord(addr, message.getServiceUUID());
                        		NameService.this.mBTTrans.getBTLiteController().foundName(message.getWkn(), message.getGuid(), addr, getPort(mmSocket));
                            }
                    		break;
                    		
                    	case NameServiceMessage.TYPE_DISCOVER_NAME_REQUEST:
                    		NameServiceMessage reply = new NameServiceMessage(NameServiceMessage.TYPE_DISCOVER_NAME_REPLEY);
                    		reply.fillDiscoverReply(mBTTrans.getBTLiteController().getGlobalGUID(), mMyServiceUUID.toString(), matchNamePrefix(message.getNamePrefix()));
    						byte[] replyArray = Utils.serializeObject(reply);
    						write(replyArray);
                    		break;
                    		
                    	case NameServiceMessage.TYPE_DISCOVER_NAME_REPLEY:
                            if(message.getWkn().length() > 0){
                            	String addr = mmSocket.getRemoteDevice().getAddress();
                            	NameService.this.mBTTrans.getBTConnectionMgr().insertServiceRercord(addr, message.getServiceUUID());
                        		NameService.this.mBTTrans.getBTLiteController().foundName(message.getWkn(), message.getGuid(), addr, getPort(mmSocket));
                            }
                    		break;
                    		
                    	default:
                    		Log.e(TAG, "Wrong NameServiceMessage type = " +  message.getType());
                    }
                    // Terminate thread
                	break;
            	}
			} catch (IOException e) {
				Log.e(TAG, "disconnected" + e.getMessage());
			}
			try {
				mmSocket.close();
			} catch (IOException e) { }
			if(mThreadType == TYPE_CLIENT){
				queryNextDevice();
			}
		}

		String getPort(BluetoothSocket sock){
			String port = null;
		    final Field fields[] = sock.getClass().getDeclaredFields();
		    String fieldName = "mPort";
		    for (int i = 0; i < fields.length; ++i) {
		      if (fieldName.equals(fields[i].getName())) {
		          fields[i].setAccessible(true);
		          try {
		        	  port = String.valueOf(fields[i].get(sock));
				} catch (IllegalArgumentException e) {
					Log.e(TAG, "getPort() IllegalArgumentException: " + e.getMessage());
				} catch (IllegalAccessException e) {
					Log.e(TAG, "getPort() IllegalAccessException: " + e.getMessage());
				}
		      }
		    }
			return port;
		}

		/**
         * Write to the connected OutStream.
         * @param buffer The bytes to write
         */
        public void write(byte[] buffer) {
            try {
                mmOutStream.write(buffer);
            } catch (IOException e) {
                Log.e(TAG, "Exception during write", e);
            }
        }
    }
}