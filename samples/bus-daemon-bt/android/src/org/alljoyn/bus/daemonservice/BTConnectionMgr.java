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
import java.net.ServerSocket;
import java.net.Socket;
import java.util.ArrayList;
import java.util.Hashtable;
import java.util.LinkedList;
import java.util.Queue;
import java.util.UUID;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;

import android.util.Log;

/**
 * Manage inter-device end point communication using Bluetooth RFCOMM implemented in Android. When a device connects to a remote device, a bluetooth connection 
 * followed by a TCP connection will be setup. The TCP-BT connection forwards data associated with each BTLiteEndpoint.. 
 */
public class BTConnectionMgr{

	private final static String TAG = "BTConnectionMgr";
	private BTLiteTransport mTrans = null;
    private BluetoothAdapter mBluetoothAdapter = null;
    private Hashtable<String, UUID> mServiceUUIDs = null;

    private static final String NAME_CONNECTION = "connectionMgr";
    private Hashtable<String, BTLiteEndpoint> mEndPointlist = new Hashtable<String, BTLiteEndpoint>();
    public static final int TCP_LISTEN_PORT = 9527;
    
	public BTConnectionMgr(BTLiteTransport trans){
		mTrans = trans;
    	mBluetoothAdapter  = BluetoothAdapter.getDefaultAdapter();
    	mServiceUUIDs = new Hashtable<String, UUID>();
		new BTListenThread().start();
	}

	/**
	 * Insert a record of service provider identified by the uuid
	 * @param addr
	 * @param uuid
	 */
	public void insertServiceRercord(String addr, String uuid){
		mServiceUUIDs.put(addr, UUID.fromString(uuid));
	}

	public void removeServiceRercord(String addr){
		mServiceUUIDs.remove(addr);
	}
	
	/**
	 * Connect to remote device for service identified by uuid
	 * @param device The remote devcie
	 * @param uuid The service UUID
	 * @return
	 */
    private BluetoothSocket getConnectedSocket(BluetoothDevice device, UUID uuid) {
        BluetoothSocket myBSock;
        try {
            myBSock = device.createRfcommSocketToServiceRecord(uuid);
            myBSock.connect();
            return myBSock;
        } catch (IOException e) {
            Log.i(TAG, "IOException in getConnectedSocket", e);
        }
        return null;
    }
  
    /**
     * Connect to a remote device specified by the specification
     * @param spec
     * @return
     */
	public String connect(String spec) {
		int index = spec.indexOf("addr=");
		int indexEnd =spec.indexOf(",port=");
		String addr = spec.substring(index + 5, indexEnd);  // Extract the MAC address
		Log.d(TAG, "Connect to address " + addr);
		BluetoothSocket mSocket = null;
		Log.d(TAG, "ConnectThread started");
		if (mBluetoothAdapter.isDiscovering()) {
			mBluetoothAdapter.cancelDiscovery();
		}
		
		BluetoothDevice device = mBluetoothAdapter.getRemoteDevice(addr);
		for (int j = 0; j < 3 && mSocket == null; j++) {
			mSocket = getConnectedSocket(device, mServiceUUIDs.get(addr));
			if (mSocket == null) {
				try {
					Thread.sleep(200);
				} catch (InterruptedException e) {
					Log.e(TAG, "InterruptedException in connect", e);
				}
			}else{
				break;
            }
		}
		if (mSocket == null) {
			return null;
        }
		UUID uuid = UUID.randomUUID(); // This UUID is used to uniquely identify the created end point
		String uniqueID = uuid.toString();
		BTLiteEndpoint b2bEndpoint = new BTLiteEndpoint(uniqueID, spec, mSocket);

		mEndPointlist.put(uniqueID, b2bEndpoint);
		return uniqueID; 
	}
	
	public BTLiteTransport getBTTransport(){
		return mTrans;
	}

	private class BTListenThread extends Thread {
		private BluetoothServerSocket mServerSocket;

		public void run() {
			BluetoothSocket socket = null;
			Log.d(TAG, "BTListenThread started");
			if (mBluetoothAdapter.isDiscovering()) {
				mBluetoothAdapter.cancelDiscovery();
			}
			try {
				while(true){
					UUID uid = BTConnectionMgr.this.mTrans.getNameSerice().getMyServiceUUID();
					Log.e(TAG, "Listen on "  + uid.toString());
					mServerSocket = mBluetoothAdapter.listenUsingRfcommWithServiceRecord(NAME_CONNECTION, uid);
					socket = mServerSocket.accept();
					mServerSocket.close();
					if (socket != null) {
						UUID uuid = UUID.randomUUID();
						String uniqueID = uuid.toString();
						BTLiteEndpoint b2bEndpoint = new BTLiteEndpoint(uniqueID, null, socket);
						mEndPointlist.put(uniqueID, b2bEndpoint);
						BTConnectionMgr.this.mTrans.getBTLiteController().accepted(uniqueID);
						Log.d(TAG,"BTListenThread create BT Endpont with UniqueID "+ uniqueID);
					}
				}
			} catch (IOException e) {
				Log.e(TAG, "BTServerSocket accept fail: " + e.getMessage());
			}
		}
	}

    /**
     * Exit the remote endpoint and remove it from hashtable
     * @param uniqueID The unique identifier to locate the remote endpoint
     */
	public void endpointExit(String uniqueID) {
		BTLiteEndpoint btEp = findEndpoint(uniqueID);
		if(btEp != null){
			btEp.exit();
			mEndPointlist.remove(uniqueID);
		}
	}
	
    public  BTLiteEndpoint findEndpoint(String uniqueID){
    	if(uniqueID == null || mEndPointlist == null) return null;
    	return mEndPointlist.get(uniqueID);
    }
}

/**
 * Counter-part of the BTLiteEndpoint class in the Native code. It serves as a bridge to forward data between the TCP and bluetooth connections. Messages received over
 * the TCP socket will be sent over the Bluetooth socket; And messages received over the bluetooth socket will be forwarded to the TCP socket
 */
class BTLiteEndpoint{
	private String connectSpec = null;
	private String uniqueName = null;             // Unique identifier of this endpoint
	private BluetoothSocket btSocket = null;      // Bluetooth socket for this endpoint
	private Socket tcpSocket = null;              // TCP socket for this endpoint
	private final static String TAG = "BTLiteEndpoint";
	
	private BTRxThread btRxThread = null;         // The thread read data from the bluetooth socket
	private BTTxThread btTxThread = null;         // The thread write data to the bluetooth socket
	private TCPRxThread tcpRxThread = null;       // The thread read data from the TCP socket
	private TCPTxThread tcpTxThread = null;       // The thread write data to the TCP socket
	private boolean bShouldExit = false;          // Whether this endpoint should stop
	
	private Queue<MsgPacket> tcpTxQueue;          // Container for pending messages to be written to TCP socket
	private Queue<MsgPacket> btTxQueue;           // Container for pending messages to be written to bluetooth socket
	
	public BTLiteEndpoint(String uname, String spec, BluetoothSocket sock){
		Log.d(TAG, "Create BTLiteEndpoint. Unique Name: " + uname + ", spec = " + spec);
		uniqueName  = uname;
		connectSpec = spec;
		btSocket = sock;

		tcpTxQueue = new LinkedList<MsgPacket>();
		btTxQueue = new LinkedList<MsgPacket>();
		
		btRxThread = new BTRxThread(btSocket);
		btRxThread.start();
		btTxThread =  new BTTxThread(btSocket);
		btTxThread.start();	
		new TCPListenThread().start();

	}
	
	public String getUniqueName(){
		return uniqueName;
	}
	
	public String getConnectSpec(){
		return connectSpec;
	}
	
	public void exit(){
		bShouldExit = true;
	}
	
    class BTRxThread extends Thread {
        private final InputStream mInStream;
    	
        public BTRxThread(BluetoothSocket socket) {
        	InputStream tmpIn = null;
        	try {
           		tmpIn = socket.getInputStream();
			} catch (IOException e) {
				Log.e(TAG, "BTRxThread IOException: " + e.getMessage());
			}
			mInStream = tmpIn;
        }
        @Override
        public void run(){
        	int buffSize = 4096;
            byte[] buffer = new byte[buffSize];
            int bytes;
        	Log.d(TAG, "BTRxThread Started");
			while (!bShouldExit) {
				try {
					// Read from the InputStream
					bytes = mInStream.read(buffer);
					Log.d(TAG, "BTRxThread Received " + bytes + " bytes");
				} catch (IOException e) {
					Log.e(TAG, "BTRxThread disconnected", e);
					if(tcpSocket != null){
						try {
							btSocket.close();
							btSocket = null;
							tcpSocket.close();
							tcpSocket = null;
						} catch (IOException e1) {}
					}
					break;
				}
				if(bytes > 0){
					byte[] payload = new byte[bytes];
					System.arraycopy(buffer, 0, payload, 0, bytes);
					MsgPacket pkt = new MsgPacket(payload, bytes);
					synchronized(BTLiteEndpoint.this){
						tcpTxQueue.add(pkt);
	        		}
				}
			}        	
        }
    }
    
    class BTTxThread extends Thread {
        private final OutputStream mOutStream;

        public BTTxThread(BluetoothSocket socket) {
    		OutputStream tmpOut = null;
        	try {
        		tmpOut = socket.getOutputStream();
			} catch (IOException e) {
				Log.e(TAG, "BTTxThread IOException: " + e.getMessage());
			}
			mOutStream = tmpOut;
        }
        
        @Override
        public void run(){
        	Log.d(TAG, "BTTxThread Started");
			while (!bShouldExit) {
				try {
					MsgPacket pkt = null;
					synchronized(BTLiteEndpoint.this){
						if(!btTxQueue.isEmpty()){
							pkt = btTxQueue.poll();
						}
					}
					if(pkt != null){
						try {
							mOutStream.write(pkt.payload, 0, pkt.getSize());
							Log.d(TAG, "BTTxThread write " + pkt.getSize());
						} catch (IOException e) {
							Log.e(TAG, "BTTxThread write exception: " + e.getMessage());
						}
					}
					sleep(30);
				} catch (InterruptedException e) {
					Log.e(TAG, "BTTxThread InterruptedException: " + e.getMessage());
				}
			}        	
        }
    }	
    
    private class TCPListenThread extends Thread {
    	ServerSocket mTcpServerSock = null;
        public TCPListenThread() {
            try {
            	mTcpServerSock = new ServerSocket(BTConnectionMgr.TCP_LISTEN_PORT);
            } catch (IOException e) {
            	Log.e(TAG, "TCPListenThread IOException: " + e.getMessage());
            }
        }
     
        @Override
        public void run() {
            Log.d(TAG, "TCPListenThread started");
            try {
            	while (true) {
            		Socket socket = mTcpServerSock.accept();
                    if (socket != null) {    
                    	tcpSocket = socket;
                    	Log.d(TAG, "TCP connection setup");
                    	tcpRxThread = new TCPRxThread(tcpSocket);
                    	tcpRxThread.start();
                    	tcpTxThread = new TCPTxThread(tcpSocket);
                    	tcpTxThread.start();
                    	break;
                    }
            	}
            } catch (IOException e) {
                	Log.e(TAG, "TCPListenThread accept() fail: " + e.getMessage());
            } finally{
            	try {
					mTcpServerSock.close();
				} catch (IOException e) {
				}
            	mTcpServerSock = null;
            }
        }
    }
    
    class TCPTxThread extends Thread {
        private OutputStream mOutStream;
    	
        public TCPTxThread(Socket socket) {
        	try {
				mOutStream = socket.getOutputStream();
			} catch (IOException e) {
				Log.e(TAG, "TCPTxThread IOException: " + e.getMessage());
			}
        }
        
        @Override
        public void run(){
        	Log.d(TAG, "TCPTxThread Started");
			while (!bShouldExit) {
				try {
					MsgPacket pkt = null;
					synchronized(BTLiteEndpoint.this){
						if(!tcpTxQueue.isEmpty()){
							pkt = tcpTxQueue.poll();
						}
					}
					if(pkt != null){
						try {
							mOutStream.write(pkt.payload, 0, pkt.getSize());
							Log.d(TAG, "TCPTxThread write " + pkt.getSize());
						} catch (IOException e) {
							Log.e(TAG, "TCPTxThread write exception: " + e.getMessage());
						}
					}
					sleep(30);
				} catch (InterruptedException e) {
					Log.e(TAG, "TCPTxThread InterruptedException: " + e.getMessage());
				}
			}     
			Log.e(TAG, "TCPTxThread end: ");
        }
    }	

    class TCPRxThread extends Thread {
        private InputStream mInStream;
    	
        public TCPRxThread(Socket socket) {
        	try {
				mInStream = socket.getInputStream();
			} catch (IOException e) {
				Log.e(TAG, "TCPRxThread IOException: " + e.getMessage());
			}
        }
        
        @Override
        public void run(){
        	Log.d(TAG, "TCPRxThread Started");
        	int buffSize = 4096;
            byte[] buffer = new byte[buffSize];
            int bytes;
			try {
				while (!bShouldExit) {
					bytes = mInStream.read(buffer);
					Log.d(TAG, "TCPRxThread TCPRxThreadReceived " + bytes + " bytes");
					if(bytes > 0){
						byte[] payload = new byte[bytes];
						System.arraycopy(buffer, 0, payload, 0, bytes);
						MsgPacket pkt = new MsgPacket(payload, bytes);
	        	
						synchronized(BTLiteEndpoint.this){
							btTxQueue.add(pkt);
						}
					}else{
						break;
					}
				}
			} catch (IOException e) {
				Log.e(TAG, "TCPRxThread disconnected" + e.getMessage());	
			}    
	    	Log.d(TAG, "TCPRxThread Exit");
			StopEndpoint();
        }
    }
    
    void StopEndpoint(){
    	if(btSocket != null){
    		try {
				tcpSocket.close();
				tcpSocket = null;
				
				btSocket.close();
				btSocket = null;
			} catch (IOException e) {
				Log.d(TAG, "StopEndpoint Close BT Socket");
			}
    	}
    }
    
    class MsgPacket{
    	private byte[] payload;
    	private int size;  
    	
    	public MsgPacket(byte[] data, int s){
    		payload = data;
    		size = s;
    	}
    	public byte[] getPayload(){
    		return payload;
    	}
    	public int getSize(){
    		return size;
    	}
    }
}
