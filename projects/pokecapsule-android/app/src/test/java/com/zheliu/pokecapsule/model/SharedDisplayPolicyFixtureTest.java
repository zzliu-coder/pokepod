package com.zheliu.pokecapsule.model;

import com.zheliu.pokecapsule.core.ProcessingState;

import org.junit.Test;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Collections;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

public final class SharedDisplayPolicyFixtureTest {
    private static final Pattern CASE = Pattern.compile("\\{\\s*\\\"name\\\".*?\\}");

    @Test public void everySharedDisplayCaseDrivesAndroidPreview() throws Exception {
        String json = new String(
                Files.readAllBytes(findFixture().toPath()), StandardCharsets.UTF_8);
        Matcher cases = CASE.matcher(json);
        int count = 0;
        while (cases.find()) {
            String item = cases.group();
            String status = stringValue(item, "status");
            CapsuleRecord record = new CapsuleRecord(
                    new File("/tmp/00000000-0000-0000-0000-000000000000"),
                    "00000000-0000-0000-0000-000000000000",
                    "",
                    "2026-08-03T00:00:00Z",
                    "2026-08-03T00:00:00Z",
                    1,
                    false,
                    Collections.emptyList(),
                    ProcessingState.fromWire(status),
                    1,
                    intValue(item, "durationMs"),
                    "",
                    false,
                    "Inbox",
                    stringValue(item, "rawText"),
                    stringValue(item, "polishedText"),
                    stringValue(item, "finalText"),
                    false,
                    "",
                    "");
            assertEquals(stringValue(item, "name"),
                    stringValue(item, "expected"), record.previewText());
            count++;
        }
        assertEquals(5, count);
    }

    private static File findFixture() {
        File cursor = new File(System.getProperty("user.dir"));
        for (int depth = 0; depth < 6 && cursor != null; depth++, cursor = cursor.getParentFile()) {
            File direct = new File(cursor, "pokecapsule-protocol/display-policy-fixtures.json");
            if (direct.isFile()) return direct;
            File projects = new File(cursor, "projects/pokecapsule-protocol/display-policy-fixtures.json");
            if (projects.isFile()) return projects;
        }
        throw new AssertionError("找不到共享展示 fixture");
    }

    private static String stringValue(String object, String key) {
        Matcher matcher = Pattern.compile("\\\"" + Pattern.quote(key)
                + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"").matcher(object);
        assertTrue("缺少字段 " + key, matcher.find());
        return matcher.group(1);
    }

    private static int intValue(String object, String key) {
        Matcher matcher = Pattern.compile("\\\"" + Pattern.quote(key)
                + "\\\"\\s*:\\s*(\\d+)").matcher(object);
        assertTrue("缺少字段 " + key, matcher.find());
        return Integer.parseInt(matcher.group(1));
    }
}
