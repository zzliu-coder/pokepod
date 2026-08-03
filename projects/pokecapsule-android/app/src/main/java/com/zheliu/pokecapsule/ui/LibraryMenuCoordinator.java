package com.zheliu.pokecapsule.ui;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.zheliu.pokecapsule.R;
import com.zheliu.pokecapsule.service.DeviceCapabilities;

import java.util.List;

/** Owns the two transient navigation surfaces used by the library page. */
@SuppressLint("SetTextI18n")
public final class LibraryMenuCoordinator {
    public enum Kind { SECTION, ITEM, DIVIDER }

    public static final class Entry {
        public final Kind kind;
        public final String label;
        public final String count;
        public final boolean child;
        public final boolean active;
        public final Runnable action;

        private Entry(Kind kind, String label, String count,
                boolean child, boolean active, Runnable action) {
            this.kind = kind;
            this.label = label;
            this.count = count;
            this.child = child;
            this.active = active;
            this.action = action;
        }

        public static Entry section(String label) {
            return new Entry(Kind.SECTION, label, "", false, false, null);
        }

        public static Entry section(String label, Runnable action) {
            return new Entry(Kind.SECTION, label, "", false, false, action);
        }

        public static Entry divider() {
            return new Entry(Kind.DIVIDER, "", "", false, false, null);
        }

        public static Entry item(String label, String count,
                boolean child, boolean active, Runnable action) {
            return new Entry(Kind.ITEM, label, count, child, active, action);
        }
    }

    private final Activity activity;
    private final FrameLayout drawerLayer;
    private final LinearLayout drawerPanel;
    private final LinearLayout drawerItems;
    private final TextView drawerSubtitle;
    private final TextView newFolder;
    private final TextView settings;
    private final FrameLayout overflowLayer;
    private final LinearLayout overflowPanel;
    private final DeviceCapabilities capabilities;

    public LibraryMenuCoordinator(Activity activity, View root) {
        this.activity = activity;
        capabilities = DeviceCapabilities.current();
        drawerLayer = root.findViewById(R.id.library_drawer_layer);
        drawerPanel = root.findViewById(R.id.library_drawer_panel);
        drawerItems = root.findViewById(R.id.library_drawer_items);
        drawerSubtitle = root.findViewById(R.id.library_drawer_subtitle);
        newFolder = root.findViewById(R.id.library_new_folder);
        settings = root.findViewById(R.id.library_open_settings);
        overflowLayer = root.findViewById(R.id.library_overflow_layer);
        overflowPanel = root.findViewById(R.id.library_overflow_panel);
        configureSurfaces();
    }

    private void configureSurfaces() {
        int surface = ViewKit.surface(activity);
        drawerPanel.setBackgroundColor(surface);
        overflowPanel.setBackgroundColor(surface);
        drawerSubtitle.setTextColor(ViewKit.secondary(activity));
        newFolder.setTextColor(ViewKit.ink(activity));
        settings.setTextColor(ViewKit.ink(activity));
        drawerLayer.setBackgroundColor(capabilities.translucentSurfaces
                ? Color.argb(72, 18, 25, 21)
                : ViewKit.background(activity));
        if (!capabilities.animatedTransitions) {
            drawerPanel.setElevation(0);
            overflowPanel.setElevation(0);
        }
        drawerLayer.setOnClickListener(view -> hideDrawer());
        drawerPanel.setOnClickListener(view -> { });
        overflowLayer.setOnClickListener(view -> hideOverflow());
        overflowPanel.setOnClickListener(view -> { });
    }

    public void showDrawer(String deviceLabel, List<Entry> entries,
            Runnable createFolder, Runnable openSettings) {
        hideOverflow();
        drawerSubtitle.setText(activity.getString(R.string.device_library_named, deviceLabel));
        newFolder.setOnClickListener(view -> {
            hideDrawer();
            createFolder.run();
        });
        settings.setOnClickListener(view -> {
            hideDrawer();
            openSettings.run();
        });
        drawerItems.removeAllViews();
        for (Entry entry : entries) addDrawerEntry(entry);
        drawerLayer.setVisibility(View.VISIBLE);
        drawerLayer.post(() -> {
            FrameLayout.LayoutParams params =
                    (FrameLayout.LayoutParams) drawerPanel.getLayoutParams();
            params.width = Math.max(dp(280), Math.round(drawerLayer.getWidth() * 0.84f));
            drawerPanel.setLayoutParams(params);
        });
    }

    public void showOverflow(List<Entry> entries) {
        hideDrawer();
        overflowPanel.removeAllViews();
        for (Entry entry : entries) {
            if (entry.kind == Kind.ITEM) addOverflowEntry(entry);
        }
        overflowLayer.setVisibility(View.VISIBLE);
    }

    public boolean closeTransientSurface() {
        if (drawerLayer.getVisibility() == View.VISIBLE) {
            hideDrawer();
            return true;
        }
        if (overflowLayer.getVisibility() == View.VISIBLE) {
            hideOverflow();
            return true;
        }
        return false;
    }

    public void hideDrawer() { drawerLayer.setVisibility(View.GONE); }
    public void hideOverflow() { overflowLayer.setVisibility(View.GONE); }

    private void addDrawerEntry(Entry entry) {
        if (entry.kind == Kind.DIVIDER) {
            View divider = new View(activity);
            divider.setBackgroundColor(ViewKit.outline(activity));
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, dp(1));
            params.setMargins(dp(18), dp(8), dp(18), dp(4));
            drawerItems.addView(divider, params);
            return;
        }
        if (entry.kind == Kind.SECTION) {
            TextView heading = ViewKit.text(activity, entry.label, 12, Typeface.BOLD);
            heading.setTextColor(ViewKit.secondary(activity));
            heading.setGravity(Gravity.CENTER_VERTICAL);
            heading.setPadding(dp(18), dp(10), dp(18), 0);
            if (entry.action != null) {
                heading.setText(entry.label + "　管理 ›");
                heading.setOnClickListener(view -> {
                    hideDrawer();
                    entry.action.run();
                });
            }
            drawerItems.addView(heading, new LinearLayout.LayoutParams(-1, dp(42)));
            return;
        }

        LinearLayout row = new LinearLayout(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(entry.child ? dp(48) : dp(18), 0, dp(18), 0);
        row.setBackgroundColor(entry.active ? ViewKit.accentSoft(activity) : Color.TRANSPARENT);
        TextView label = ViewKit.text(activity, entry.label, 16,
                entry.active ? Typeface.BOLD : Typeface.NORMAL);
        TextView count = ViewKit.text(activity, entry.count, 13, Typeface.NORMAL);
        count.setTextColor(ViewKit.secondary(activity));
        count.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        row.addView(label, new LinearLayout.LayoutParams(0, -1, 1f));
        row.addView(count, new LinearLayout.LayoutParams(dp(54), -1));
        row.setOnClickListener(view -> {
            hideDrawer();
            if (entry.action != null) entry.action.run();
        });
        drawerItems.addView(row, new LinearLayout.LayoutParams(-1, dp(50)));
        if (capabilities.eink) {
            View divider = new View(activity);
            divider.setBackgroundColor(ViewKit.outline(activity));
            drawerItems.addView(divider, new LinearLayout.LayoutParams(-1, dp(1)));
        }
    }

    private void addOverflowEntry(Entry entry) {
        TextView row = ViewKit.text(activity, entry.label, 16, Typeface.NORMAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(18), 0, dp(18), 0);
        row.setOnClickListener(view -> {
            hideOverflow();
            if (entry.action != null) entry.action.run();
        });
        overflowPanel.addView(row, new LinearLayout.LayoutParams(-1, dp(52)));
        View divider = new View(activity);
        divider.setBackgroundColor(ViewKit.outline(activity));
        overflowPanel.addView(divider, new LinearLayout.LayoutParams(-1, dp(1)));
    }

    private int dp(int value) { return ViewKit.dp(activity, value); }
}
