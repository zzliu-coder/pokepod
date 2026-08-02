package com.zheliu.pokecapsule.transcription;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public final class TencentAsrConfigTest {
    @Test public void transferExportCanBeParsedAgain() {
        TencentAsrConfig original = TencentAsrConfig.parse(
                "SecretId=AKIDexample123\nSecretKey=secretExample456\n");

        TencentAsrConfig imported = TencentAsrConfig.parse(original.exportForTransfer());

        assertEquals(original.secretId, imported.secretId);
        assertEquals(original.secretKey, imported.secretKey);
    }
}
