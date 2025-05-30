/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.lib.crash.notification

import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import androidx.core.app.NotificationManagerCompat
import androidx.test.ext.junit.runners.AndroidJUnit4
import mozilla.components.lib.crash.Crash
import mozilla.components.lib.crash.CrashReporter
import mozilla.components.support.test.robolectric.testContext
import mozilla.components.support.test.whenever
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.mockito.Mockito.spy
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config

@RunWith(AndroidJUnit4::class)
class CrashNotificationTest {
    @Test
    fun shouldShowNotificationInsteadOfPrompt() {
        val foregroundChildNativeCrash = Crash.NativeCodeCrash(
            timestamp = 0,
            minidumpPath = "",
            extrasPath = "",
            processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_FOREGROUND_CHILD,
            processType = "content",
            breadcrumbs = arrayListOf(),
            remoteType = null,
        )

        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(foregroundChildNativeCrash, sdkLevel = 21))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(foregroundChildNativeCrash, sdkLevel = 22))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(foregroundChildNativeCrash, sdkLevel = 23))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(foregroundChildNativeCrash, sdkLevel = 24))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(foregroundChildNativeCrash, sdkLevel = 25))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(foregroundChildNativeCrash, sdkLevel = 26))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(foregroundChildNativeCrash, sdkLevel = 27))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(foregroundChildNativeCrash, sdkLevel = 28))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(foregroundChildNativeCrash, sdkLevel = 29))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(foregroundChildNativeCrash, sdkLevel = 30))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(foregroundChildNativeCrash, sdkLevel = 31))

        val mainProcessNativeCrash = Crash.NativeCodeCrash(
            timestamp = 0,
            minidumpPath = "",
            extrasPath = "",
            processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_MAIN,
            processType = "main",
            breadcrumbs = arrayListOf(),
            remoteType = null,
        )

        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(mainProcessNativeCrash, sdkLevel = 21))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(mainProcessNativeCrash, sdkLevel = 22))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(mainProcessNativeCrash, sdkLevel = 23))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(mainProcessNativeCrash, sdkLevel = 24))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(mainProcessNativeCrash, sdkLevel = 25))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(mainProcessNativeCrash, sdkLevel = 26))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(mainProcessNativeCrash, sdkLevel = 27))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(mainProcessNativeCrash, sdkLevel = 28))
        assertTrue(CrashNotification.shouldShowNotificationInsteadOfPrompt(mainProcessNativeCrash, sdkLevel = 29))
        assertTrue(CrashNotification.shouldShowNotificationInsteadOfPrompt(mainProcessNativeCrash, sdkLevel = 30))
        assertTrue(CrashNotification.shouldShowNotificationInsteadOfPrompt(mainProcessNativeCrash, sdkLevel = 31))

        val backgroundChildNativeCrash = Crash.NativeCodeCrash(
            timestamp = 0,
            minidumpPath = "",
            extrasPath = "",
            processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_BACKGROUND_CHILD,
            processType = "utility",
            breadcrumbs = arrayListOf(),
            remoteType = null,
        )

        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(backgroundChildNativeCrash, sdkLevel = 21))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(backgroundChildNativeCrash, sdkLevel = 22))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(backgroundChildNativeCrash, sdkLevel = 23))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(backgroundChildNativeCrash, sdkLevel = 24))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(backgroundChildNativeCrash, sdkLevel = 25))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(backgroundChildNativeCrash, sdkLevel = 26))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(backgroundChildNativeCrash, sdkLevel = 27))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(backgroundChildNativeCrash, sdkLevel = 28))
        assertTrue(CrashNotification.shouldShowNotificationInsteadOfPrompt(backgroundChildNativeCrash, sdkLevel = 29))
        assertTrue(CrashNotification.shouldShowNotificationInsteadOfPrompt(backgroundChildNativeCrash, sdkLevel = 30))
        assertTrue(CrashNotification.shouldShowNotificationInsteadOfPrompt(backgroundChildNativeCrash, sdkLevel = 31))

        val exceptionCrash = Crash.UncaughtExceptionCrash(0, RuntimeException("Boom"), arrayListOf())

        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(exceptionCrash, sdkLevel = 21))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(exceptionCrash, sdkLevel = 22))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(exceptionCrash, sdkLevel = 23))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(exceptionCrash, sdkLevel = 24))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(exceptionCrash, sdkLevel = 25))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(exceptionCrash, sdkLevel = 26))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(exceptionCrash, sdkLevel = 27))
        assertFalse(CrashNotification.shouldShowNotificationInsteadOfPrompt(exceptionCrash, sdkLevel = 28))
        assertTrue(CrashNotification.shouldShowNotificationInsteadOfPrompt(exceptionCrash, sdkLevel = 29))
        assertTrue(CrashNotification.shouldShowNotificationInsteadOfPrompt(exceptionCrash, sdkLevel = 30))
        assertTrue(CrashNotification.shouldShowNotificationInsteadOfPrompt(exceptionCrash, sdkLevel = 31))
    }

    @Test
    fun `Showing notification`() {
        val notificationManager = testContext.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val shadowNotificationManager = shadowOf(notificationManager)

        assertEquals(0, shadowNotificationManager.notificationChannels.size)
        assertEquals(0, shadowNotificationManager.size())

        val crash = Crash.UncaughtExceptionCrash(0, RuntimeException("Boom"), arrayListOf())
        val notificationManagerCompat = spy(NotificationManagerCompat.from(testContext))

        whenever(notificationManagerCompat.areNotificationsEnabled()).thenReturn(true)

        val crashNotification = CrashNotification(
            testContext,
            crash,
            CrashReporter.PromptConfiguration(
                appName = "TestApp",
            ),
        )
        crashNotification.show()

        assertEquals(1, shadowNotificationManager.notificationChannels.size)
        assertEquals(
            "Crashes",
            (shadowNotificationManager.notificationChannels[0] as NotificationChannel).name,
        )

        assertEquals(1, shadowNotificationManager.size())
    }

    @Test
    @Config(sdk = [32])
    fun `not showing notification when permission is denied on SDK 32 and below`() {
        val notificationManager = testContext.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val shadowNotificationManager = shadowOf(notificationManager)

        assertEquals(0, shadowNotificationManager.notificationChannels.size)
        assertEquals(0, shadowNotificationManager.size())

        val crash = Crash.UncaughtExceptionCrash(0, RuntimeException("Boom"), arrayListOf())
        val notificationManagerCompat = spy(NotificationManagerCompat.from(testContext))

        whenever(notificationManagerCompat.areNotificationsEnabled()).thenReturn(false)

        val crashNotification = CrashNotification(
            testContext,
            crash,
            CrashReporter.PromptConfiguration(
                appName = "TestApp",
            ),
            notificationManagerCompat,
        )
        crashNotification.show()

        assertEquals(1, shadowNotificationManager.notificationChannels.size)
        assertEquals(
            "Crashes",
            (shadowNotificationManager.notificationChannels[0] as NotificationChannel).name,
        )

        assertEquals(0, shadowNotificationManager.size())
    }

    @Test
    fun `not showing notification when permission is needed and denied`() {
        val notificationManager = testContext.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val shadowNotificationManager = shadowOf(notificationManager)

        assertEquals(0, shadowNotificationManager.notificationChannels.size)
        assertEquals(0, shadowNotificationManager.size())

        val crash = Crash.UncaughtExceptionCrash(0, RuntimeException("Boom"), arrayListOf())
        val notificationManagerCompat = spy(NotificationManagerCompat.from(testContext))

        whenever(notificationManagerCompat.areNotificationsEnabled()).thenReturn(false)

        val crashNotification = CrashNotification(
            testContext,
            crash,
            CrashReporter.PromptConfiguration(
                appName = "TestApp",
            ),
            notificationManagerCompat,
        )
        crashNotification.show()

        assertEquals(1, shadowNotificationManager.notificationChannels.size)
        assertEquals(
            "Crashes",
            (shadowNotificationManager.notificationChannels[0] as NotificationChannel).name,
        )

        assertEquals(0, shadowNotificationManager.size())
    }
}
