// Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

package com.whitelabs.anc

import android.app.*
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat

class AncService : Service() {

    private external fun startEngine()
    private external fun stopEngine()

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val action = intent?.action
        if (action == "STOP") {
            stopEngine()
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        } else {
            createNotification()
            startEngine()
        }
        return START_STICKY
    }

    private fun createNotification() {
        val channelId = "ANC_CHANNEL"
        val channel = NotificationChannel(
            channelId,
            "ANC Service",
            NotificationManager.IMPORTANCE_LOW
        )
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(channel)

        val stopIntent = Intent(this, AncService::class.java).apply { action = "STOP" }
        val stopPending = PendingIntent.getService(this, 0, stopIntent, PendingIntent.FLAG_IMMUTABLE)

        val notification = NotificationCompat.Builder(this, channelId)
            .setContentTitle("Active Noise Cancellation")
            .setContentText("The silence engine is running.")
            .setSmallIcon(android.R.drawable.ic_lock_silent_mode)
            .addAction(android.R.drawable.ic_media_pause, "Turn Off", stopPending)
            .build()

        startForeground(1, notification)
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        init {
            System.loadLibrary("anc-lib")
        }
    }
}
