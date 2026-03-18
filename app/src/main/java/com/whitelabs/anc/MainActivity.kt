// Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

package com.whitelabs.anc

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.whitelabs.anc.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val handler = Handler(Looper.getMainLooper())
    private var isRunning = false
    private var logPfd: ParcelFileDescriptor? = null

    private external fun getFftData(): FloatArray
    private external fun enableLogging(fd: Int)
    private external fun disableLogging()

    private val requestMicPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) startAnc()
        }

    private val pickLogFile =
        registerForActivityResult(ActivityResultContracts.CreateDocument("text/plain")) { uri: Uri? ->
            if (uri == null) {
                binding.switchLogging.isChecked = false
                return@registerForActivityResult
            }
            openLogUri(uri)
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
                pickLogFile.launch("anc_dev.log")
            } else {
                stopLogging()
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

    private fun openLogUri(uri: Uri) {
        try {
            val pfd = contentResolver.openFileDescriptor(uri, "wa")
                ?: throw IllegalStateException("Could not open file descriptor")
            logPfd?.close()
            logPfd = pfd
            enableLogging(pfd.fd)
        } catch (e: Exception) {
            binding.switchLogging.isChecked = false
            logPfd = null
        }
    }

    private fun stopLogging() {
        disableLogging()
        logPfd?.close()
        logPfd = null
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

    override fun onDestroy() {
        super.onDestroy()
        stopLogging()
    }

    companion object {
        init {
            System.loadLibrary("anc-lib")
        }
    }
}
