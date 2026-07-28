package com.zheliu.pokecapsule.ui;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.TextView;

final class ViewKit {
    private ViewKit() {}

    static TextView text(Context context, String value, int size, int style) {
        TextView view = new TextView(context);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(Color.BLACK);
        view.setTypeface(Typeface.DEFAULT, style);
        view.setGravity(Gravity.CENTER_VERTICAL);
        return view;
    }

    static TextView button(Context context, String value, View.OnClickListener action) {
        TextView view = text(context, value, 17, Typeface.BOLD);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(context, 10), dp(context, 8), dp(context, 10), dp(context, 8));
        GradientDrawable box = new GradientDrawable();
        box.setColor(Color.WHITE);
        box.setStroke(dp(context, 1), Color.BLACK);
        box.setCornerRadius(dp(context, 4));
        view.setBackground(box);
        view.setOnClickListener(action);
        return view;
    }

    static int dp(Context context, int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }
}
