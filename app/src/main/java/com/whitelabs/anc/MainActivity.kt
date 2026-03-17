// Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

package com.whitelabs.anc

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.whitelabs.anc.databinding.ActivityMainBinding
import java.io.File

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val handler = Handler(Looper.getMainLooper())
    private var isRunning = false

    private external fun getFftData(): FloatArray
    private external fun enableLogging(fd: Int)
    private external fun disableLogging()

    private val requestMicPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) startAnc()
        }

    private val updateVisualizer = object : Runnable {
        override fun run() {
            if (isRunning) {
                binding.visualizerView.updateData(getFftData())
                handler.postDelayed(this, 16)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.switchLogging.setOnCheckedChangeListener { _, isChecked ->
            if (isChecked) {
                val logFile = File(getExternalFilesDir(null), "anc_dev.log")
                val pfd = android.os.ParcelFileDescriptor.open(
                    logFile,
                    android.os.ParcelFileDescriptor.MODE_CREATE or
                    android.os.ParcelFileDescriptor.MODE_WRITE_ONLY or
                    android.os.ParcelFileDescriptor.MODE_APPEND
                )
                enableLogging(pfd.fd)
                pfd.detachFd()
            } else {
                disableLogging()
            }
        }

        binding.btnToggleAnc.setOnClickListener {
            if (!isRunning) {
                if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
                    != PackageManager.PERMISSION_GRANTED
                ) {
                    requestMicPermission.launch(Manifest.permission.RECORD_AUDIO)
                } else {
                    startAnc()
                }
            } else {
                stopAnc()
            }
        }
    }

    private fun startAnc() {
        startForegroundService(Intent(this, AncService::class.java))
        isRunning = true
        binding.btnToggleAnc.text = "Stop ANC"
        handler.post(updateVisualizer)
    }

    private fun stopAnc() {
        startService(Intent(this, AncService::class.java).apply { action = "STOP" })
        isRunning = false
        binding.btnToggleAnc.text = "Ignite ANC"
    }

    companion object {
        init {
            System.loadLibrary("anc-lib")
        }
    }
}
