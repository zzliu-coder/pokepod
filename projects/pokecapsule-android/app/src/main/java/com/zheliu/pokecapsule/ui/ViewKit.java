package com.zheliu.pokecapsule.ui;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.zheliu.pokecapsule.service.DeviceRuntimeProfile;

final class ViewKit {
    static final int COLOR_BACKGROUND = Color.rgb(245, 246, 244);
    static final int COLOR_SURFACE = Color.WHITE;
    static final int COLOR_INK = Color.rgb(23, 26, 24);
    static final int COLOR_SECONDARY = Color.rgb(98, 103, 98);
    static final int COLOR_ACCENT = Color.rgb(49, 95, 82);
    static final int COLOR_ACCENT_SOFT = Color.rgb(226, 236, 231);
    static final int COLOR_ERROR = Color.rgb(162, 59, 50);
    static final int COLOR_ERROR_SOFT = Color.rgb(247, 232, 229);
    static final int COLOR_OUTLINE = Color.rgb(217, 221, 217);

    private ViewKit() {}

    static TextView text(Context context, String value, int size, int style) {
        TextView view = new TextView(context);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(ink(context));
        view.setTypeface(Typeface.DEFAULT, style);
        view.setGravity(Gravity.CENTER_VERTICAL);
        return view;
    }

    static TextView button(Context context, String value, View.OnClickListener action) {
        return secondaryButton(context, value, action);
    }

    static TextView primaryButton(Context context, String value, View.OnClickListener action) {
        TextView view = control(context, value, action);
        view.setTextColor(Color.WHITE);
        view.setBackground(box(context, accent(context), accent(context), 12));
        return view;
    }

    static TextView secondaryButton(Context context, String value, View.OnClickListener action) {
        TextView view = control(context, value, action);
        view.setBackground(box(context, accentSoft(context), Color.TRANSPARENT, 12));
        return view;
    }

    static TextView quietButton(Context context, String value, View.OnClickListener action) {
        TextView view = control(context, value, action);
        view.setTypeface(Typeface.DEFAULT, Typeface.NORMAL);
        view.setBackground(box(context, surface(context), outline(context), 12));
        return view;
    }

    static TextView status(Context context, String value, boolean error) {
        TextView view = text(context, value, 13, Typeface.BOLD);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(context, 10), dp(context, 5), dp(context, 10), dp(context, 5));
        view.setTextColor(error ? error(context) : accent(context));
        view.setBackground(box(
                context,
                error ? errorSoft(context) : accentSoft(context),
                Color.TRANSPARENT,
                999));
        return view;
    }

    static LinearLayout card(Context context) {
        LinearLayout card = new LinearLayout(context);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(
                dp(context, 16), dp(context, 14), dp(context, 16), dp(context, 14));
        card.setBackground(box(context, surface(context), outline(context), 14));
        return card;
    }

    static TextView sectionTitle(Context context, String value) {
        TextView view = text(context, value, 18, Typeface.BOLD);
        view.setPadding(0, 0, 0, dp(context, 8));
        return view;
    }

    static int background(Context context) {
        return DeviceRuntimeProfile.isLowPowerReader() ? Color.WHITE : COLOR_BACKGROUND;
    }

    static int surface(Context context) {
        return Color.WHITE;
    }

    static int ink(Context context) {
        return DeviceRuntimeProfile.isLowPowerReader() ? Color.BLACK : COLOR_INK;
    }

    static int secondary(Context context) {
        return DeviceRuntimeProfile.isLowPowerReader() ? Color.DKGRAY : COLOR_SECONDARY;
    }

    static int accent(Context context) {
        return DeviceRuntimeProfile.isLowPowerReader() ? Color.BLACK : COLOR_ACCENT;
    }

    static int accentSoft(Context context) {
        return DeviceRuntimeProfile.isLowPowerReader()
                ? Color.rgb(235, 235, 235)
                : COLOR_ACCENT_SOFT;
    }

    static int error(Context context) {
        return DeviceRuntimeProfile.isLowPowerReader() ? Color.BLACK : COLOR_ERROR;
    }

    static int errorSoft(Context context) {
        return DeviceRuntimeProfile.isLowPowerReader()
                ? Color.rgb(235, 235, 235)
                : COLOR_ERROR_SOFT;
    }

    static int outline(Context context) {
        return DeviceRuntimeProfile.isLowPowerReader() ? Color.BLACK : COLOR_OUTLINE;
    }

    private static TextView control(Context context, String value, View.OnClickListener action) {
        TextView view = text(context, value, 15, Typeface.BOLD);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(context, 10), dp(context, 8), dp(context, 10), dp(context, 8));
        view.setMinHeight(dp(context, 48));
        view.setOnClickListener(action);
        return view;
    }

    private static GradientDrawable box(
            Context context, int fill, int stroke, int radiusDp) {
        GradientDrawable box = new GradientDrawable();
        box.setColor(fill);
        if (stroke != Color.TRANSPARENT) box.setStroke(dp(context, 1), stroke);
        box.setCornerRadius(dp(
                context, DeviceRuntimeProfile.isLowPowerReader() ? Math.min(radiusDp, 8) : radiusDp));
        return box;
    }

    static int dp(Context context, int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }
}
