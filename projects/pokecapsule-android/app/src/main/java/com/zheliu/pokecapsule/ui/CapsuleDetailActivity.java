package com.zheliu.pokecapsule.ui;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Color;
import android.graphics.Typeface;
import android.media.MediaPlayer;
import android.os.Bundle;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.zheliu.pokecapsule.core.ProcessingState;
import com.zheliu.pokecapsule.service.DeviceRuntimeProfile;
import com.zheliu.pokecapsule.service.LibraryChangeNotifier;
import com.zheliu.pokecapsule.model.CapsuleRecord;
import com.zheliu.pokecapsule.service.TranscriptionScheduler;
import com.zheliu.pokecapsule.storage.CapsuleStore;
import com.zheliu.pokecapsule.storage.PokePaths;
import com.zheliu.pokecapsule.transcription.TencentAsrConfig;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class CapsuleDetailActivity extends Activity {
    private final CapsuleStore store = new CapsuleStore(new PokePaths());
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private String capsuleId;
    private CapsuleRecord record;
    private TextView title;
    private TextView metadata;
    private TextView raw;
    private TextView polished;
    private TextView finalText;
    private MediaPlayer player;
    private boolean libraryReceiverRegistered;

    private final BroadcastReceiver libraryChangeReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            load();
            if (TencentAsrConfig.isConfigured(CapsuleDetailActivity.this)) {
                TranscriptionScheduler.scheduleAutomatic(CapsuleDetailActivity.this);
            }
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        capsuleId = getIntent().getStringExtra("capsuleId");
        setContentView(buildPage());
    }

    @Override public void onResume() {
        super.onResume();
        load();
        if (TencentAsrConfig.isConfigured(this)) {
            TranscriptionScheduler.scheduleAutomatic(this);
        }
    }

    @Override public void onStart() {
        super.onStart();
        registerReceiver(
                libraryChangeReceiver,
                new IntentFilter(LibraryChangeNotifier.ACTION),
                LibraryChangeNotifier.INTERNAL_PERMISSION,
                null);
        libraryReceiverRegistered = true;
    }

    @Override public void onStop() {
        if (libraryReceiverRegistered) {
            unregisterReceiver(libraryChangeReceiver);
            libraryReceiverRegistered = false;
        }
        super.onStop();
    }

    @Override public void onDestroy() {
        stopPlayback();
        io.shutdownNow();
        super.onDestroy();
    }

    private ScrollView buildPage() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(18), dp(14), dp(18), dp(20));
        page.setBackgroundColor(Color.WHITE);
        scroll.addView(page);

        title = ViewKit.text(this, "胶囊", 27, Typeface.BOLD);
        page.addView(title, lp(-1, dp(54)));
        metadata = ViewKit.text(this, "读取中…", 15, Typeface.NORMAL);
        metadata.setPadding(0, 0, 0, dp(10));
        page.addView(metadata);

        LinearLayout first = row();
        first.addView(ViewKit.button(this, "播放/停止", v -> togglePlayback()), weight());
        first.addView(ViewKit.button(this, "改标题", v -> promptTitle()), weight());
        first.addView(ViewKit.button(this, "收藏", v -> toggleFavorite()), weight());
        page.addView(first, lp(-1, dp(52)));

        LinearLayout second = row();
        second.addView(ViewKit.button(this, "加标签", v -> promptTag(true)), weight());
        second.addView(ViewKit.button(this, "移标签", v -> promptTag(false)), weight());
        second.addView(ViewKit.button(this, "重试转写", v -> retry()), weight());
        page.addView(second, lp(-1, dp(52)));

        LinearLayout third = row();
        third.addView(ViewKit.button(this, "编辑最终文字", v -> promptFinalText()), weight());
        third.addView(ViewKit.button(this, "复制最终文字", v -> copyFinalText()), weight());
        page.addView(third, lp(-1, dp(52)));

        page.addView(ViewKit.text(this, "原始转写", 20, Typeface.BOLD), lp(-1, dp(44)));
        raw = ViewKit.text(this, "尚未生成", 17, Typeface.NORMAL);
        raw.setTextIsSelectable(true);
        page.addView(raw);

        page.addView(ViewKit.text(this, "校对文字", 20, Typeface.BOLD), lp(-1, dp(44)));
        polished = ViewKit.text(this, "尚未生成", 17, Typeface.NORMAL);
        polished.setTextIsSelectable(true);
        page.addView(polished);

        page.addView(ViewKit.text(this, "最终文字", 20, Typeface.BOLD), lp(-1, dp(44)));
        finalText = ViewKit.text(this, "尚未编辑", 17, Typeface.NORMAL);
        finalText.setTextIsSelectable(true);
        page.addView(finalText);
        return scroll;
    }

    private void load() {
        io.execute(() -> {
            try {
                File directory = store.paths().findCapsuleById(capsuleId);
                if (directory == null) throw new Exception("胶囊已移动或不存在");
                CapsuleRecord loaded = store.readCapsule(directory);
                String rawText = readOptional(new File(directory, "raw.txt"));
                String polishedText = readOptional(new File(directory, "polished.md"));
                String finalValue = readOptional(new File(directory, "final.md"));
                runOnUiThread(() -> {
                    record = loaded;
                    title.setText(loaded.title);
                    metadata.setText(loaded.metadataText()
                            + (loaded.favorite ? "\n★ 已收藏" : "")
                            + (loaded.tagsText().isEmpty() ? "" : "\n" + loaded.tagsText())
                            + (loaded.error.isEmpty() ? "" : "\n" + loaded.error));
                    raw.setText(rawText.isEmpty() ? "尚未生成" : rawText);
                    polished.setText(polishedText.isEmpty() ? "尚未生成" : polishedText);
                    finalText.setText(finalValue.isEmpty() ? "尚未编辑" : finalValue);
                });
            } catch (Exception error) {
                runOnUiThread(() -> toast(error.getMessage()));
            }
        });
    }

    private void promptFinalText() {
        if (record == null || record.readOnly) return;
        String initial = record.finalText;
        if (initial == null || initial.isEmpty()) {
            initial = !record.polishedText.isEmpty() ? record.polishedText : record.rawText;
        }
        EditText input = new EditText(this);
        input.setText(initial);
        input.setMinLines(6);
        input.setGravity(android.view.Gravity.TOP);
        new AlertDialog.Builder(this)
                .setTitle("编辑最终文字")
                .setView(input)
                .setPositiveButton("保存", (dialog, which) ->
                        runOperation(
                                () -> store.setFinalText(record.id, input.getText().toString()),
                                "最终文字已保存"))
                .setNegativeButton("取消", null)
                .show();
    }

    private void copyFinalText() {
        if (record == null) return;
        String value = record.previewText();
        android.content.ClipboardManager clipboard =
                (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(android.content.ClipData.newPlainText("PokeCapsule", value));
        toast("文字已复制");
    }

    private void togglePlayback() {
        if (record == null) return;
        if (player != null) {
            stopPlayback();
            toast("已停止播放");
            return;
        }
        File audio = new File(record.directory, "audio.m4a");
        try {
            player = new MediaPlayer();
            player.setDataSource(audio.getAbsolutePath());
            player.setOnCompletionListener(value -> stopPlayback());
            player.prepare();
            player.start();
            toast(DeviceRuntimeProfile.isLowPowerReader()
                    ? "正在播放；Poke3 需连接蓝牙或 USB 音频设备"
                    : "正在通过手机扬声器播放");
        } catch (Exception error) {
            stopPlayback();
            toast("无法播放: " + error.getMessage());
        }
    }

    private void promptTitle() {
        if (record == null || record.readOnly) return;
        prompt("修改标题", record.title, value ->
                runOperation(() -> store.setTitle(record.id, value), "标题已更新"));
    }

    private void toggleFavorite() {
        if (record == null || record.readOnly) return;
        runOperation(
                () -> store.setFavorite(Collections.singletonList(record.id), !record.favorite),
                record.favorite ? "已取消收藏" : "已收藏");
    }

    private void promptTag(boolean add) {
        if (record == null || record.readOnly) return;
        prompt(add ? "添加标签" : "移除标签", "", value ->
                runOperation(() -> {
                    if (add) store.addTag(Collections.singletonList(record.id), value);
                    else store.removeTag(Collections.singletonList(record.id), value);
                }, "标签已更新"));
    }

    private void retry() {
        if (record == null || record.readOnly) return;
        if (record.status != ProcessingState.FAILED) {
            toast("当前状态无需重试");
            return;
        }
        runOperation(() -> {
            store.updateProcessing(record.directory, ProcessingState.QUEUED,
                    null, null, false);
            TranscriptionScheduler.scheduleManual(this);
        }, "已重新排队");
    }

    private void runOperation(Operation operation, String message) {
        io.execute(() -> {
            try {
                operation.run();
                runOnUiThread(() -> {
                    toast(message);
                    load();
                });
            } catch (Exception error) {
                runOnUiThread(() -> toast("操作失败: " + error.getMessage()));
            }
        });
    }

    private void prompt(String heading, String initial, TextResult callback) {
        EditText input = new EditText(this);
        input.setText(initial);
        input.setSelection(input.length());
        new AlertDialog.Builder(this)
                .setTitle(heading)
                .setView(input)
                .setPositiveButton("确定", (dialog, which) -> callback.accept(input.getText().toString()))
                .setNegativeButton("取消", null)
                .show();
    }

    private void stopPlayback() {
        if (player != null) {
            try {
                player.stop();
            } catch (IllegalStateException ignored) {
            }
            player.release();
            player = null;
        }
    }

    private static String readOptional(File file) throws Exception {
        if (!file.isFile()) return "";
        try (FileInputStream input = new FileInputStream(file);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            return output.toString(StandardCharsets.UTF_8.name());
        }
    }

    private static String tagsLine(CapsuleRecord record) {
        if (record.tags.isEmpty()) return "无标签";
        StringBuilder result = new StringBuilder();
        for (String tag : record.tags) {
            if (result.length() > 0) result.append(' ');
            result.append('#').append(tag);
        }
        return result.toString();
    }

    private LinearLayout row() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        return row;
    }

    private LinearLayout.LayoutParams weight() {
        LinearLayout.LayoutParams value = new LinearLayout.LayoutParams(0, -1, 1f);
        value.setMargins(dp(2), dp(3), dp(2), dp(3));
        return value;
    }

    private LinearLayout.LayoutParams lp(int width, int height) {
        return new LinearLayout.LayoutParams(width, height);
    }

    private int dp(int value) {
        return ViewKit.dp(this, value);
    }

    private void toast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
    }

    private interface Operation {
        void run() throws Exception;
    }

    private interface TextResult {
        void accept(String value);
    }
}
