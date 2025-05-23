package mozilla.components.feature.search.icons

import mozilla.appservices.remotesettings.RemoteSettingsClient
import mozilla.components.support.remotesettings.RemoteSettingsService
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.mockito.Mock
import org.mockito.Mockito
import org.mockito.Mockito.mock
import org.mockito.junit.MockitoJUnitRunner

@RunWith(MockitoJUnitRunner::class)
class SearchConfigIconsUpdateServiceTest {

    @Mock
    private lateinit var mockRemoteSettingsService: RemoteSettingsService
    private lateinit var mockClient: RemoteSettingsClient

    private lateinit var service: SearchConfigIconsUpdateService
    private lateinit var spyService: SearchConfigIconsUpdateService

    @Before
    fun setup() {
        mockClient = mock()
        service = SearchConfigIconsUpdateService(mockClient)
        spyService = Mockito.spy(service)
    }

    @Test
    fun `Given valid records When fetchIcons is called Then parsed data is returned`() {
        val expectedModels = listOf(
            SearchConfigIconsModel(
                schema = 1L,
                imageSize = 64,
                attachment = AttachmentModel(
                    filename = "icon.png",
                    mimetype = "image/png",
                    location = "location",
                    hash = "hash123",
                    size = 1024u,
                ),
                engineIdentifier = listOf("google"),
                filterExpression = "",
            ),
        )

        Mockito.doReturn(expectedModels).`when`(spyService).fetchIconsRecords(mockRemoteSettingsService)

        val result = spyService.fetchIconsRecords(mockRemoteSettingsService)

        assertEquals(expectedModels.size, result.size)
        assertEquals(expectedModels[0], result[0])
    }

    @Test
    fun `Given no valid records When fetchIcons is called Then empty list is returned`() {
        Mockito.doReturn(emptyList<SearchConfigIconsModel>()).`when`(spyService).fetchIconsRecords(mockRemoteSettingsService)

        val result = spyService.fetchIconsRecords(mockRemoteSettingsService)

        assertTrue(result.isEmpty())
    }
}
