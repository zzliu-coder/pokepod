package com.zheliu.pokecapsule.transcription;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public final class TencentAsrTest {
    @Test public void parsesDocumentStyleCredentials() {
        TencentAsrConfig value = TencentAsrConfig.parse(
                "SecretId: AKIDexample123\nSecretKey = exampleSecret456\n");
        assertEquals("AKIDexample123", value.secretId);
        assertEquals("exampleSecret456", value.secretKey);
    }

    @Test(expected = IllegalArgumentException.class)
    public void rejectsIncompleteCredentials() {
        TencentAsrConfig.parse("SecretId: AKIDexample123");
    }

    @Test public void tc3AuthorizationIsStableAndDoesNotExposeSecretKey() throws Exception {
        TencentAsrConfig value = new TencentAsrConfig(
                "AKIDexample123", "exampleSecret456");
        String first = TencentAsrClient.authorization("{}", 1_700_000_000L, value);
        String second = TencentAsrClient.authorization("{}", 1_700_000_000L, value);
        assertEquals(first, second);
        assertTrue(first.contains("Credential=AKIDexample123/"));
        assertTrue(first.contains("SignedHeaders=content-type;host;x-tc-action"));
        assertFalse(first.contains("exampleSecret456"));
    }
}
