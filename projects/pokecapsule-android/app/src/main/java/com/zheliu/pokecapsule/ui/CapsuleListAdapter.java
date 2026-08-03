package com.zheliu.pokecapsule.ui;

import android.content.Context;
import android.graphics.Typeface;
import android.text.TextUtils;
import android.view.View;
import android.view.ViewGroup;
import android.view.LayoutInflater;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.zheliu.pokecapsule.model.CapsuleRecord;
import com.zheliu.pokecapsule.R;

import java.util.ArrayList;
import java.util.Set;

final class CapsuleListAdapter extends ArrayAdapter<CapsuleRecord> {
    private final Set<String> selection;

    CapsuleListAdapter(Context context, Set<String> selection) {
        super(context, android.R.layout.simple_list_item_1, new ArrayList<>());
        this.selection = selection;
    }

    @Override public View getView(int position, View convertView, ViewGroup parent) {
        CapsuleRecord record = getItem(position);
        LinearLayout row = convertView instanceof LinearLayout
                ? (LinearLayout) convertView
                : (LinearLayout) LayoutInflater.from(parent.getContext())
                        .inflate(R.layout.row_capsule, parent, false);
        row.setBackgroundColor(ViewKit.surface(parent.getContext()));
        TextView preview = row.findViewById(R.id.capsule_preview);
        TextView metadata = row.findViewById(R.id.capsule_metadata);
        preview.setTextColor(ViewKit.ink(parent.getContext()));
        metadata.setTextColor(ViewKit.secondary(parent.getContext()));
        int previewFormat = selection.contains(record.id)
                ? (record.favorite ? R.string.capsule_preview_selected_favorite
                        : R.string.capsule_preview_selected)
                : (record.favorite ? R.string.capsule_preview_favorite
                        : R.string.capsule_preview_plain);
        preview.setText(getContext().getString(previewFormat, record.previewText()));
        metadata.setText(record.tagsText().isEmpty()
                ? record.metadataText()
                : getContext().getString(R.string.metadata_with_tags,
                        record.metadataText(), record.tagsText()));
        return row;
    }

}
