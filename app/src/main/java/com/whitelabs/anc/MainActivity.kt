// Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

package com.whitelabs.anc

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.whitelabs.anc.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val handler = Handler(Looper.getMainLooper())
    private var isRunning = false

    // JNI: poll FFT data from C++ engine
    private external fun getFftData(): FloatArray

    private val updateVisualizer = object : Runnable {
        override fun run() {
            if (isRunning) {
                val data = getFftData()
                binding.visualizerView.updateData(data)
                handler.postDelayed(this, 16)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Prefer Bluetooth / wired headset microphone
        val audioManager = getSystemService(AUDIO_SERVICE) as AudioManager
        audioManager.isBluetoothScoOn = true

        binding.btnToggleAnc.setOnClickListener {
            if (!isRunning) {
                if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
                    != PackageManager.PERMISSION_GRANTED
                ) {
                    requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), 101)
                } else {
                    startAnc()
                }
            } else {
                stopAnc()
            }
        }
    }

    private fun startAnc() {
        val intent = Intent(this, AncService::class.java)
        startForegroundService(intent)
        isRunning = true
        binding.btnToggleAnc.text = "Stop ANC"
        handler.post(updateVisualizer)
    }

    private fun stopAnc() {
        val intent = Intent(this, AncService::class.java).apply { action = "STOP" }
        startService(intent)
        isRunning = false
        binding.btnToggleAnc.text = "Ignite ANC"
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 101 && grantResults.isNotEmpty() &&
            grantResults[0] == PackageManager.PERMISSION_GRANTED
        ) {
            startAnc()
        }
    }

    companion object {
        init {
            System.loadLibrary("anc-lib")
        }
    }
}
