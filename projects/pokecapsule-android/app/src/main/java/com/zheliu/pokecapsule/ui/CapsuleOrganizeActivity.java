package com.zheliu.pokecapsule.ui;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.zheliu.pokecapsule.core.PathPolicy;
import com.zheliu.pokecapsule.model.CapsuleRecord;
import com.zheliu.pokecapsule.storage.LibraryRepository;

import java.util.ArrayList;
import java.util.Collections;

/** A full-page organizer, keeping structural actions out of transient dialogs. */
@SuppressLint("SetTextI18n")
public final class CapsuleOrganizeActivity extends Activity {
    private LibraryRepository repository;
    private String capsuleId;
    private CapsuleRecord record;
    private TextView folderRow;
    private TextView tagRow;
    private TextView favoriteRow;
    private TextView titleRow;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        repository = new LibraryRepository(this, message -> toast("操作失败：" + message));
        capsuleId = getIntent().getStringExtra("capsuleId");
        setContentView(buildPage());
    }

    @Override public void onResume() {
        super.onResume();
        load();
    }

    @Override protected void onDestroy() {
        repository.close();
        super.onDestroy();
    }

    private View buildPage() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(ViewKit.background(this));
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(18), dp(18), dp(18), dp(28));
        page.setBackgroundColor(ViewKit.background(this));
        scroll.addView(page);

        TextView back = ViewKit.text(this, "‹ 返回胶囊", 16, Typeface.BOLD);
        back.setOnClickListener(view -> finish());
        page.addView(back, lp(-1, dp(44)));
        page.addView(ViewKit.text(this, "整理胶囊", 28, Typeface.BOLD), lp(-1, dp(58)));
        TextView summary = ViewKit.text(this,
                "目录确定位置，标签用于交叉归类，收藏用于快速找到。", 14, Typeface.NORMAL);
        summary.setTextColor(ViewKit.secondary(this));
        page.addView(summary, lp(-1, dp(52)));

        titleRow = settingRow("标题", "读取中…", view -> editTitle());
        folderRow = settingRow("移动到目录", "读取中…", view -> chooseFolder());
        tagRow = settingRow("标签", "读取中…", view -> editTags());
        favoriteRow = settingRow("收藏", "读取中…", view -> toggleFavorite());
        page.addView(titleRow, lp(-1, dp(64)));
        page.addView(folderRow, lp(-1, dp(64)));
        page.addView(tagRow, lp(-1, dp(64)));
        page.addView(favoriteRow, lp(-1, dp(64)));
        return scroll;
    }

    private TextView settingRow(String title, String value, View.OnClickListener action) {
        TextView row = ViewKit.text(this, title + "\n" + value + "　›", 16, Typeface.NORMAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), 0, dp(12), 0);
        row.setBackground(ViewKit.outlinedBackground(this));
        row.setOnClickListener(action);
        return row;
    }

    private void load() {
        repository.loadRecord(capsuleId, loaded -> {
                    record = loaded;
                    titleRow.setText("标题\n" + loaded.title + "　›");
                    folderRow.setText("移动到目录\n" + displayFolder(loaded.relativeFolder) + "　›");
                    tagRow.setText("标签\n" + (loaded.tagsText().isEmpty()
                            ? "无标签" : loaded.tagsText()) + "　›");
                    favoriteRow.setText("收藏\n" + (loaded.favorite ? "已收藏" : "未收藏") + "　›");
        });
    }

    private void chooseFolder() {
        if (record == null) return;
        repository.loadFolders(values -> {
                ArrayList<String> folders = new ArrayList<>();
                folders.add(PathPolicy.INBOX);
                folders.add(PathPolicy.ARCHIVE);
                folders.addAll(values);
                Collections.sort(folders.subList(2, folders.size()));
                String[] labels = new String[folders.size()];
                for (int index = 0; index < folders.size(); index++) {
                    labels[index] = displayFolder(folders.get(index));
                }
                new AlertDialog.Builder(this)
                        .setTitle("移动到目录")
                        .setItems(labels, (dialog, which) -> repository.move(
                                record, folders.get(which),
                                () -> operationFinished("已移动")))
                        .setNegativeButton("取消", null)
                        .show();
        });
    }

    private void editTitle() {
        if (record == null) return;
        prompt("修改标题", record.title,
                value -> repository.setTitle(
                        record, value, () -> operationFinished("标题已更新")));
    }

    private void editTags() {
        if (record == null) return;
        prompt("标签", "", value -> new AlertDialog.Builder(this)
                .setTitle("#" + PathPolicy.normalizeTag(value))
                .setItems(new String[]{"添加", "移除"}, (dialog, which) -> {
                    Runnable success = () -> operationFinished("标签已更新");
                    if (which == 0) repository.addTag(record, value, success);
                    else repository.removeTag(record, value, success);
                })
                .show());
    }

    private void toggleFavorite() {
        if (record == null) return;
        boolean wasFavorite = record.favorite;
        repository.setFavorite(record, !wasFavorite,
                () -> operationFinished(wasFavorite ? "已取消收藏" : "已收藏"));
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

    private void operationFinished(String message) {
        toast(message);
        load();
    }

    private static String displayFolder(String folder) {
        if (PathPolicy.INBOX.equals(folder)) return "收件箱";
        if (PathPolicy.ARCHIVE.equals(folder)) return "归档";
        return folder;
    }

    private LinearLayout.LayoutParams lp(int width, int height) {
        LinearLayout.LayoutParams value = new LinearLayout.LayoutParams(width, height);
        value.bottomMargin = dp(8);
        return value;
    }

    private int dp(int value) { return ViewKit.dp(this, value); }
    private void toast(String text) { Toast.makeText(this, text, Toast.LENGTH_LONG).show(); }
    private interface TextResult { void accept(String value); }
}
