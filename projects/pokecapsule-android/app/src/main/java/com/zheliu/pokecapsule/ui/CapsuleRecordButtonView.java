package com.zheliu.pokecapsule.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.View;

public final class CapsuleRecordButtonView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF capsule = new RectF();
    private final RectF meter = new RectF();
    private boolean recording;
    private int secondsLeft;
    private int audioLevel;
    private boolean silenceWarning;

    public CapsuleRecordButtonView(Context context) {
        super(context);
        setContentDescription("开始录音");
    }

    public void showIdle(String message) {
        recording = false;
        audioLevel = 0;
        silenceWarning = false;
        setContentDescription(message == null || message.isEmpty() ? "开始录音" : message);
        invalidate();
    }

    public void showRecording(int seconds, int level, boolean silent) {
        recording = true;
        secondsLeft = seconds;
        audioLevel = Math.max(0, Math.min(3, level));
        silenceWarning = silent;
        setContentDescription(silent
                ? "录音中，未检测到声音，剩余 " + seconds + " 秒"
                : "录音中，剩余 " + seconds + " 秒，音量 " + audioLevel + " 级");
        invalidate();
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float cx = getWidth() / 2f;
        float cy = getHeight() / 2f;
        float coreRadius = dp(23);

        paint.setStyle(Paint.Style.FILL);
        paint.setColor(recording ? Color.BLACK : Color.WHITE);
        canvas.drawCircle(cx, cy, coreRadius, paint);

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(2));
        paint.setColor(Color.BLACK);
        canvas.drawCircle(cx, cy, coreRadius, paint);

        if (recording) {
            drawRecording(canvas, cx, cy);
        } else {
            drawCapsule(canvas, cx, cy);
        }
    }

    private void drawCapsule(Canvas canvas, float cx, float cy) {
        capsule.set(cx - dp(14), cy - dp(7), cx + dp(14), cy + dp(7));
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(3));
        paint.setColor(Color.BLACK);
        canvas.drawRoundRect(capsule, dp(7), dp(7), paint);
        canvas.drawLine(cx, cy - dp(6), cx, cy + dp(6), paint);
    }

    private void drawRecording(Canvas canvas, float cx, float cy) {
        meter.set(cx - dp(29), cy - dp(29), cx + dp(29), cy + dp(29));
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(2.5f));
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setColor(Color.BLACK);
        for (int i = 0; i < audioLevel; i++) {
            canvas.drawArc(meter, 205 + i * 50, 34, false, paint);
        }
        paint.setStrokeCap(Paint.Cap.BUTT);

        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.WHITE);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(dp(secondsLeft < 10 ? 20 : 18));
        paint.setFakeBoldText(true);
        float baseline = cy - (paint.ascent() + paint.descent()) / 2f;
        canvas.drawText(String.valueOf(secondsLeft), cx, baseline, paint);
        paint.setFakeBoldText(false);

        if (silenceWarning) {
            paint.setStrokeWidth(dp(2));
            canvas.drawLine(cx - dp(8), cy + dp(13), cx + dp(8), cy + dp(13), paint);
        }
    }

    private float dp(float value) {
        return value * getResources().getDisplayMetrics().density;
    }
}
