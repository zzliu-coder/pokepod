package com.zheliu.pokecapsule.ui;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Toast;

import com.zheliu.pokecapsule.service.DeviceCapabilities;
import com.zheliu.pokecapsule.service.OverlayService;
import com.zheliu.pokecapsule.service.TranscriptionPolicyText;
import com.zheliu.pokecapsule.service.TranscriptionScheduler;
import com.zheliu.pokecapsule.transcription.TencentAsrConfig;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class SettingsActivity extends Activity {
    private static final int REQUEST_TENCENT_CONFIG = 201;
    private final ExecutorService io = Executors.newSingleThreadExecutor();

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setTitle("设置");
        setContentView(buildPage());
    }

    @Override protected void onDestroy() {
        io.shutdownNow();
        super.onDestroy();
    }

    private View buildPage() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(18), dp(16), dp(18), dp(28));
        page.setBackgroundColor(ViewKit.background(this));
        addSection(page, "录音", "录音权限与录音入口", this::openApplicationSettings);
        addSection(page, "自动转写",
                TencentAsrConfig.isConfigured(this)
                        ? "腾讯转写已配置 · " + TranscriptionPolicyText.shortCondition()
                        : "腾讯转写待配置",
                this::transcriptionActions);
        addSection(page, "电脑连接", "USB 调试、设备连接与文件管理", this::openDeviceSettings);
        addSection(page, "存储与备份", "原始录音永久保留；删除先进入回收站",
                this::openApplicationSettings);
        addSection(page, "诊断信息", DeviceCapabilities.current().eink
                        ? "Poke3 · 黑白即时刷新 · 系统悬浮录音"
                        : "Android 手机 · 移动网络可转写 · 内置扬声器播放",
                this::showDiagnostics);
        if (DeviceCapabilities.current().systemOverlayRecorder) {
            addSection(page, "悬浮录音胶囊", "开启、隐藏或停用系统悬浮入口",
                    this::overlayActions);
        }
        scroll.addView(page);
        return scroll;
    }

    private void addSection(LinearLayout page, String title, String summary, Runnable action) {
        LinearLayout card = ViewKit.card(this);
        card.addView(ViewKit.sectionTitle(this, title));
        card.addView(ViewKit.text(this, summary, 15, android.graphics.Typeface.NORMAL));
        card.setOnClickListener(view -> action.run());
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.bottomMargin = dp(12);
        page.addView(card, params);
    }

    private void transcriptionActions() {
        boolean configured = TencentAsrConfig.isConfigured(this);
        new AlertDialog.Builder(this)
                .setTitle("自动转写")
                .setItems(new String[]{configured ? "重新导入腾讯配置" : "导入腾讯配置",
                        "立即处理一条排队胶囊"}, (dialog, which) -> {
                    if (which == 0) openTencentConfigPicker();
                    else {
                        TranscriptionScheduler.scheduleManual(this);
                        toast("已提交转写任务");
                    }
                })
                .show();
    }

    private void overlayActions() {
        new AlertDialog.Builder(this)
                .setTitle("悬浮录音胶囊")
                .setItems(new String[]{"开启", "临时隐藏", "彻底关闭"}, (dialog, which) -> {
                    if (which == 0) enableOverlay();
                    else sendOverlay(which == 1 ? OverlayService.ACTION_HIDE : OverlayService.ACTION_DISABLE);
                })
                .show();
    }

    private void showDiagnostics() {
        DeviceCapabilities capabilities = DeviceCapabilities.current();
        new AlertDialog.Builder(this)
                .setTitle("设备能力")
                .setMessage("墨水屏：" + yesNo(capabilities.eink)
                        + "\n应用内录音：" + yesNo(capabilities.inlineRecorder)
                        + "\n系统悬浮录音：" + yesNo(capabilities.systemOverlayRecorder)
                        + "\n移动网络转写：" + yesNo(capabilities.cellularTranscription)
                        + "\n内置扬声器播放：" + yesNo(capabilities.directSpeakerPlayback))
                .setPositiveButton("关闭", null)
                .show();
    }

    private void openTencentConfigPicker() {
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("text/plain");
        startActivityForResult(picker, REQUEST_TENCENT_CONFIG);
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_TENCENT_CONFIG || resultCode != RESULT_OK
                || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        io.execute(() -> {
            try (InputStream input = getContentResolver().openInputStream(uri);
                 ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                if (input == null) throw new IOException("无法读取配置文件");
                byte[] buffer = new byte[4096];
                int count;
                while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
                TencentAsrConfig.save(this, TencentAsrConfig.parse(
                        new String(output.toByteArray(), StandardCharsets.UTF_8)));
                TranscriptionScheduler.scheduleAutomatic(this);
                runOnUiThread(() -> toast("腾讯转写已配置"));
            } catch (Exception error) {
                runOnUiThread(() -> toast("配置失败：" + error.getMessage()));
            }
        });
    }

    private void openDeviceSettings() {
        try {
            if (DeviceCapabilities.current().eink) {
                Intent onyx = new Intent("com.onyx.action.SETTING");
                onyx.setPackage("com.onyx");
                startActivity(onyx);
            } else {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS));
            }
        } catch (Exception error) {
            startActivity(new Intent(Settings.ACTION_SETTINGS));
        }
    }

    private void openApplicationSettings() {
        startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.parse("package:" + getPackageName())));
    }

    private void enableOverlay() {
        if (!Settings.canDrawOverlays(this)) {
            startActivity(new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            return;
        }
        sendOverlay(null);
    }

    private void sendOverlay(String action) {
        Intent service = new Intent(this, OverlayService.class);
        if (action != null) service.setAction(action);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(service);
        else startService(service);
    }

    private static String yesNo(boolean value) { return value ? "是" : "否"; }
    private int dp(int value) { return ViewKit.dp(this, value); }
    private void toast(String text) { Toast.makeText(this, text, Toast.LENGTH_SHORT).show(); }
}
