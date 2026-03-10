// Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

package com.whitelabs.anc

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View

class VisualizerView(context: Context, attrs: AttributeSet) : View(context, attrs) {

    private val paint = Paint().apply {
        color = android.graphics.Color.CYAN
        strokeWidth = 5f
        style = Paint.Style.FILL
    }

    private var magnitudes = FloatArray(0)

    fun updateData(newMagnitudes: FloatArray) {
        magnitudes = newMagnitudes
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val widthPerBar = width.toFloat() / magnitudes.size
        for (i in magnitudes.indices) {
            val barHeight = magnitudes[i] * height / magnitudes.size
            canvas.drawRect(
                i * widthPerBar,
                height - barHeight,
                (i + 1) * widthPerBar,
                height.toFloat(),
                paint
            )
        }
    }
}
