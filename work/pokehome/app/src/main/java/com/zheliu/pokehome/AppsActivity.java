package com.zheliu.pokehome;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.List;

public final class AppsActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(buildList());
    }

    private View buildList() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(28), dp(24), dp(28), dp(24));
        page.setBackgroundColor(Color.WHITE);
        scroll.addView(page);

        TextView title = label("我的应用", 29, Typeface.BOLD);
        page.addView(title, params(-1, dp(60)));

        PackageManager manager = getPackageManager();
        Intent launcherQuery = new Intent(Intent.ACTION_MAIN, null);
        launcherQuery.addCategory(Intent.CATEGORY_LAUNCHER);
        List<ResolveInfo> apps = manager.queryIntentActivities(launcherQuery, 0);
        int count = 0;
        for (ResolveInfo info : apps) {
            String packageName = info.activityInfo.packageName;
            ApplicationInfo application = info.activityInfo.applicationInfo;
            boolean system = (application.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
            if (system || packageName.equals(getPackageName())) continue;
            TextView item = button(info.loadLabel(manager).toString(), v -> {
                Intent launch = manager.getLaunchIntentForPackage(packageName);
                if (launch != null) startActivity(launch);
            });
            page.addView(item, params(-1, dp(98)));
            addGap(page, 12);
            count++;
        }
        if (count == 0) page.addView(label("这里会显示你后来安装的应用。", 18, Typeface.NORMAL));
        return scroll;
    }

    private void addGap(LinearLayout page, int height) {
        View gap = new View(this);
        page.addView(gap, params(-1, dp(height)));
    }

    private TextView button(String value, View.OnClickListener listener) {
        TextView item = label(value, 23, Typeface.BOLD);
        item.setGravity(Gravity.CENTER_VERTICAL);
        item.setPadding(dp(24), 0, dp(24), 0);
        GradientDrawable box = new GradientDrawable();
        box.setColor(Color.WHITE);
        box.setStroke(dp(2), Color.BLACK);
        box.setCornerRadius(dp(8));
        item.setBackground(box);
        item.setOnClickListener(listener);
        return item;
    }

    private TextView label(String value, int size, int style) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(Color.BLACK);
        view.setTypeface(Typeface.DEFAULT, style);
        return view;
    }

    private LinearLayout.LayoutParams params(int width, int height) {
        return new LinearLayout.LayoutParams(width, height);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
