package com.zheliu.pokecapsule.core;

import java.text.Normalizer;
import java.util.Locale;

public final class PathPolicy {
    public static final String INBOX = "Inbox";
    public static final String ARCHIVE = "Archive";

    private PathPolicy() {}

    public static String normalizeName(String input) {
        return input == null ? "" : Normalizer.normalize(input.trim(), Normalizer.Form.NFC);
    }

    public static boolean isValidFolderName(String input) {
        String name = normalizeName(input);
        if (name.isEmpty() || name.length() > 80 || name.startsWith(".")) return false;
        if (name.equals(".") || name.equals("..") || isReserved(name)) return false;
        for (int i = 0; i < name.length(); i++) {
            char value = name.charAt(i);
            if (value == '/' || value == '\\' || value == 0 || Character.isISOControl(value)) {
                return false;
            }
        }
        return true;
    }

    public static boolean isValidTag(String input) {
        String tag = normalizeTag(input);
        if (tag.isEmpty() || tag.length() > 50) return false;
        for (int i = 0; i < tag.length(); i++) {
            if (Character.isISOControl(tag.charAt(i))) return false;
        }
        return true;
    }

    public static String normalizeTag(String input) {
        String tag = normalizeName(input);
        while (tag.startsWith("#")) tag = tag.substring(1).trim();
        return Normalizer.normalize(tag, Normalizer.Form.NFC);
    }

    public static boolean isReserved(String name) {
        String lower = normalizeName(name).toLowerCase(Locale.ROOT);
        return lower.equals(INBOX.toLowerCase(Locale.ROOT))
                || lower.equals(ARCHIVE.toLowerCase(Locale.ROOT))
                || lower.equals(".staging")
                || lower.equals(".locks")
                || lower.equals(".commands")
                || lower.equals(".trash");
    }

    public static boolean isSafeRelativeFolder(String relative) {
        if (relative == null || relative.isEmpty()) return true;
        String[] parts = relative.split("/", -1);
        if (parts.length > 2) return false;
        for (String part : parts) {
            if (!isValidFolderName(part)) return false;
        }
        return true;
    }

    public static String normalizeRelativeFolder(String relative) {
        if (relative == null) return "";
        String[] parts = relative.trim().split("/", -1);
        StringBuilder output = new StringBuilder();
        for (String part : parts) {
            String normalized = normalizeName(part);
            if (normalized.isEmpty()) continue;
            if (output.length() > 0) output.append('/');
            output.append(normalized);
        }
        return output.toString();
    }
}
