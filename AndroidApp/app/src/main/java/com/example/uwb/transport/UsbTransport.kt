package com.example.uwb.transport

import android.content.Context
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.util.Log
import com.hoho.android.usbserial.driver.CdcAcmSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.hoho.android.usbserial.util.SerialInputOutputManager

class UsbTransport(private val context: Context) : Transport {

    private var port: UsbSerialPort? = null
    private var ioManager: SerialInputOutputManager? = null
    private var connected = false
    private val permissionHelper = UsbPermissionHelper(context)
    private var dataCallback: ((ByteArray) -> Unit)? = null

    /**
     * Mở device ESP32. Nếu chưa có permission thì xin, sau đó callback onConnected.
     */
    fun openDevice(device: UsbDevice, onConnected: (() -> Unit)? = null) {
        val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        if (!usbManager.hasPermission(device)) {
            permissionHelper.requestPermission(device) {
                initDevice(device, usbManager)
                if (connected) onConnected?.invoke()
            }
        } else {
            initDevice(device, usbManager)
            if (connected) onConnected?.invoke()
        }
    }

    /**
     * Khởi tạo cổng serial CDC/ACM cho ESP32-S3.
     * Dùng usb-serial-for-android để xử lý CDC protocol tự động.
     * Nếu default prober không nhận, fallback sang CdcAcmSerialDriver thủ công.
     */
    private fun initDevice(device: UsbDevice, usbManager: UsbManager) {
        // Thử tìm driver qua default prober trước
        val drivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
        val driver = drivers.find { it.device.deviceId == device.deviceId }
            ?: CdcAcmSerialDriver(device)  // fallback: ép CDC/ACM cho ESP32 native USB

        val connection = usbManager.openDevice(driver.device) ?: run {
            Log.e("UsbTransport", "Không mở được USB connection")
            return
        }

        try {
            port = driver.ports[0]
            port?.open(connection)
            // Baud rate 115200 khớp với Serial.begin(115200) trong code ESP32
            port?.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            connected = true
            Log.d("UsbTransport", "ESP32 kết nối thành công: ${device.deviceName}")
        } catch (e: Exception) {
            Log.e("UsbTransport", "Không init được port: ${e.message}")
            try { port?.close() } catch (_: Exception) {}
            port = null
            connected = false
        }
    }

    val isConnected: Boolean get() = connected

    override fun connect() {
        // Không dùng — openDevice() đảm nhận
    }

    /**
     * Gửi lệnh tới ESP32 (ví dụ: "1" bật LED, "0" tắt LED).
     * Chạy trên background thread để không block UI.
     */
    override fun send(data: ByteArray) {
        if (!connected || port == null) {
            Log.w("UsbTransport", "Chưa kết nối, không gửi được")
            return
        }
        Thread {
            try {
                port?.write(data, 1000)
                Log.d("UsbTransport", "Đã gửi: ${String(data)}")
            } catch (e: Exception) {
                Log.e("UsbTransport", "Lỗi gửi: ${e.message}")
            }
        }.start()
    }

    /**
     * Cập nhật callback nhận dữ liệu từ ESP32.
     * Nếu SerialInputOutputManager chưa chạy thì khởi động lần đầu.
     * Các lần gọi tiếp theo chỉ đổi callback, không restart manager.
     */
    override fun receive(callback: (ByteArray) -> Unit) {
        if (!connected || port == null) return
        dataCallback = callback
        if (ioManager == null) {
            ioManager = SerialInputOutputManager(port, object : SerialInputOutputManager.Listener {
                override fun onNewData(data: ByteArray) {
                    dataCallback?.invoke(data)
                }

                override fun onRunError(e: Exception) {
                    Log.e("UsbTransport", "Lỗi nhận: ${e.message}")
                    connected = false
                    ioManager = null
                }
            })
            ioManager?.start()
        }
    }

    override fun disconnect() {
        connected = false
        ioManager?.stop()
        ioManager = null
        try { port?.close() } catch (_: Exception) {}
        port = null
        permissionHelper.unregisterReceiver()
        Log.d("UsbTransport", "Đã ngắt kết nối")
    }
}
