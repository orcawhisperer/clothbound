#pragma once

#include "audio/localspeech.h"
#include "audio/speech.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>

class ClothboundTests : public QObject {
    Q_OBJECT

private:
    static void write(const QString &path, const QByteArray &contents);
    static QByteArray png(int width, int height);
    static QByteArray fixture(const QString &name);
    static QByteArray fontPayload();
    static QByteArray adobeObfuscate(QByteArray data);
    static QByteArray packsManifest(const QString &base, const QByteArray &engineSha = "00",
                                    const QByteArray &piperSha = "00", qint64 engineSize = 10,
                                    qint64 piperSize = 10);
    static void fakeInstall(const QString &root, const QString &dir);
    static SpeechBlock block(qint64 start, const QString &text, const QString &kind = "text");

    // An engine that makes a tone: half a second per sentence at 16 kHz.
    class ToneEngine : public TtsEngine {
    public:
        std::shared_ptr<QStringList> spoken = std::make_shared<QStringList>();
        QString prepare(const VoicePacks::Model &) override { return {}; }
        int sampleRate() const override { return 16000; }
        QVector<float> speak(const QString &text, int, const std::atomic_bool &stop) override;
    };

private slots:
    void speechFindsSentences();
    void speechLeavesOutNoteMarkers();
    void speechSaysHeadingsWell();
    void speechMakesClips();
    void speechBeginsAClipWhereTheMoodChanges();
    void speechSplitsLongParagraphs();
    void speechTimelineCoversTheClip();
    void stretcherKeepsPitchAtSpeed();
    void stretcherWaitsForAudioStillArriving();
    void audioCacheKeepsClips();
    void themeFallsBack();
    void themeTokensKeepTextReadableInEveryOmarchyTheme();
    void themeAppearanceCanOverrideTheDesktop();
    void themeReadsThePalette();
    void themeFollowsChanges();
    void themeFollowsHyprlandRounding();
    void translatorFallsBackToEnglish();
    void translatorFollowsTheLanguageFile();
    void translatorPicksPluralForms();
    void shippedCatalogsAreValidJson();
    void zipReadsStoredAndDeflatedEntries();
    void zipReadsZip64();
    void zipRejectsOtherFiles();
    void tokenizerDecodesEntities();
    void tokenizerDecodesDeclaredEncodings();
    void treeParsesForgivingly();
    void textExtractionMatchesTheReader();
    void linksResolveInsideTheBook();
    void epub2ReadsPackageAndNcx();
    void epub3ReadsNavigationAndRefines();
    void epubWithEncryptedTextIsDrm();
    void epubWithoutChaptersFails();
    void bookInfoReadsEpubMetadataAndCover();
    void pdfReadsMetadataAndRenders();
    void unreadableFilesReportWhy();
    void libraryStoresBooksAndReadingState();
    void libraryGroupsEditionsIntoWorks();
    void schemeServesWellFormedBooksAsXml();
    void readerMapsPositionsOntoTheBook();
    void marksAreKeptAndUndone();
    void voicePacksOfferTheRightVoices();
    void voicePacksInstallFromTheNetwork();
    void resamplingKeepsPitchAndLength();
    void builtinVoicesSpeakASentenceAtATime();
    void localChatBodies();
    void localAnswersAreCleaned();
    void localModelsStream();
    void studioVoicesSpeak();
    void serversAreFound();
    void featuresAreRouted();
    void computerIsInspected();
    void companionReadsOnlySoFar();
    void passagesAreFound();
    void passagesAreFoundByMeaning();
    void geminiEmbeddingRequests();
    void pdfPagesAreSpoken();
    void pdfTextIsPlaced();
    void pdfPagesTakeTheTheme();
    void coverProviderBakesCornersAndSpineCrease();
    void castLinesAreRead();
    void audiobookMetadata();
    void keyArrivesWhenTheKeyringAnswers();
    void voicesAreSpokenByTheirOwnProvider();
    void searchIgnoresCaseAccentsAndQuotes();
    void searchShowsAHitInContext();
    void searchFindsWordsAcrossTheBook();
    void sseParserHandlesChunkedStreams();
    void geminiBuildsTheDocumentedRequests();
    void geminiReadsRecordedResponses();
    void geminiStreamsTextAndSpeech();
    void ledgerPricesByDateAndTier();
    void servicesKeepOnlyWorkingKeysAndCountCosts();
    void voicesSuggestNarratorsForTheBook();
    void voicesAreKeptForADay();
    void voicesCastADesignedNarrator();
    void fileNamesFollowTheCollectionPattern();
    void authorsReadTheSameEveryWay();
    void titlesClean();
    void editionsShareAWorkKey();
    void identityResolvesSwapsAndMislabels();
    void instanceHandsOver();
    void instanceFocusesHyprlandOverSocketWithoutMutatingEnv();
    void notifierTracksTokensPerIdAndCleansClosed();
    void pointerDriverSendsPublicEvents();
    void cliCommandsHandleEdgeCases();
    void pdfPagesConcurrentCancelAndRapidBookSwitch();
    void pdfBookOversizedAndExtremeAspectClamped();
    void readerPdfSpeakableIsAsyncAndDropsStaleSession();
    void htmlDeepNestingParsesAndDestroysIteratively();
    void zipRejectsBombsBadCrcAndZip64Wraparound();
    void bookSchemeEnforcesSecurityHeadersRootRelativeAndXhtmlEntities();
    void libraryRelinksMovedFilesByFingerprintAndRollsBackFailedTransactions();
    void marksMarkdownUsesValidUtf8BookmarkIconAndGuardsClosedDb();
    void bookSearchNormalizesPdfAndEpubTextAllowsSingleCjkAndCancelsMidIndex();
    void companionStripsMarkdownImagesAndHtmlImg();
    void aiServicesLocalOverrideNeverFallsBackToCloud();
    void aiServicesBudgetZeroStrictVsUnlimitedAndAskRollback();
    void geminiPcmFromWavGuards32BitWraparound();
    void localModelsStripsChunkSplitThinkTagsAndRejectsNonHttpUrls();
    void passageIndexSplitsMinifiedSingleLineEpub();
    void narrationNullProviderRecutAndChapterEndSleepCancel();
    void castIsolatesVoiceFamiliesAcrossNarratorSwitches();
    void audioCacheConcurrentWritesAndCorruptDecodeEviction();
    void stretcherFlushesShortClipsAndTailsAtNonUnitRate();
    void localReplyRecordsBoundaryBeforeAudioReceived();
    void audiobookResolvesCoverArtAndSanitizesFilename();
    void readerPerBookAppearanceRoundTripAndIsolation();
    void pdfBookPageLinksAndSelectWordBounds();
    void readerRtlDirectionJumpBackAndWpmAutomatedExclusion();
    void companionPersistsThreadsWhosWhoCascadesAndGuardsSpoilers();
    void hybridSearchRrfSentenceSpanAndSpoilerFree();
    void localModelsNormalizesProxyUrlsAndParsesContextWindow();
    void speechHeadingWhitelistAndDialogueSpanSplitting();
    void speechPlayerOffThreadWsolaAndMidPlaybackRateRewind();
    void narrationSkipSecondsSleepFadeRestoreAndPendingStart();
    void directorPdfBatchRangeAndSplitMoodsByPage();
    void audiobookSynthesizesPauseSilenceAndPartialChapterSuffix();
    void meaningSearchSpansTheLibrary();
    void libraryIndexFindsWordsAcrossBooks();
    void libraryFinderOffersFoldersWithBooks();
    void libraryShelvesWantToReadListeningAndChapter();
    void librarySchemaMigrationsCollectionsStatsAndSeriesSort();
    void marksW3cQuoteHealingCrossBookSearchAndObsidianExport();
    void cliVersionFastPathExitsCleanlyWithoutDisplay();
    void surfaceWatcherMergesChromiumFlagsCleanly();
    void surfaceWatcherHandlesExposeHyprlandAndSceneGraph();
    void surfaceWatcherCoalescesReturnNudgesAndHyprBursts();
    void readerSurfaceRefreshAvoidsElidedOutlineAndNoOpRelayout();
    void dropCapOpensTheFirstRealParagraph();
    void readerJsSuppressesNudgeRelayoutAndAllowsRealResize();
    void wave2LibraryCancelScanAudioShelvesFinderAndIndexScoping();
    void wave2ReaderBookPagesPreviousOpenedAtAndSurfaceProtections();
    void wave2NarrationScrubberVoiceAccentAndAudioBadgesPersistence();
    void wave2CompanionMigrationWhereLocalSuggestionsAiServicesAndSystemCheck();
    void wave2QmlComponentsCompileCleanlyAndListColumnsAligned();
    void wave2TransitionsDualFramePdfPrefetchAndSfx();
    void wave2VoiceNarrationSleepFadeAndEmptyLeadGuard();
    void wave2SettingsAiLocalSetupAudioCacheVoicePacksAndFolders();
    void wave2CompanionRecapsFirstSeenResumeSdcvSseGeminiIllustratorAndTranslator();
    void migrationMovesGenuineLegacyDataAndRenamesVoiceSentinels();
    void migrationRejectsForeignDogearDirectories();
    void migrationNeverClobbersExistingClothboundData();
    void wave4MultimodalPayloadsAndVisionErrors();
    void wave4AdaptiveReadingModePersistenceAndGlossary();
    void wave4SmartScopeSpoilerGuardAndShelfCitations();
    void wave4OfflineLatexMathAndSsrfPreservation();
    void wave4VisualSnipEntryAndPrivacyBudgetInvariants();
    void wave4PdfRegionSnipClampingAndThreadSafety();
    void wave4PdfLinksDestYAndPeekDestination();
    void wave4PdfAcademicIdExtractionAndCitations();
    void wave4McpProtocolKillSwitchToolsAndResources();
    void wave4CliSearchPassageAndMcpStdioPurity();
    void wave5PdfLineBandsAndRealPageTurnSfx();
    void wave5SlimListenBarAndToastClearance();
    void wave5CompanionResponseSpeechAndResize();
    void wave5LibraryCollectionsRecentAndSidebarIcons();
    void smoothWheelScrollPhysicsAndLibraryWiring();
    void articlesShelfAndDesktopV040Wiring();
    void pdfPresetViewModes();
    void cacheDirectoryDeletionSelfHeals();
    void trayIconAndShortcutToggleWiring();
    void pdfPagesRawCacheAndDownscaleReuse();
    void smoothWheelTrackpadAccelerationAndKineticCoast();
    void mprisMetadataAndForcedAsserts();
    void fixReaderUiReaderJsRecoveryClobberAndPerf();
    void fixReaderUiQmlSafetyLifecyclePanningAndPerf();
    void fixCoreMigrationRewritesPathsAndSafeCopy();
    void fixCoreZipArchiveHardeningLimitsAndOverlap();
    void fixCoreLibraryFutureSchemaAndTargetedSavePosition();
    void fixCoreLibraryIndexRebuildCancelAndIndexedPassage();
    void fixCorePdfPagesReleaseAndSystemCheckNullGuard();
    void fixAudioScopedCastCancelDoesNotDeadlockSecondCaller();
    void fixAudioNarrationHoldSamplesReleasesOnShortClipsAndSleepTrim();
    void fixAudioNarrationQueueBlockedDuringResponseAndPauseSleepClear();
    void fixAudioCacheClearStoreRaceAndWavValidation();
    void fixAudioAudiobookSnapshotsMetadataAcrossBookSwitch();
    void fixAudioVoicePacksCachedIsInstalledAndSpeechPlayerChunks();
    void fixAiMcpCompanionSanitizerAndSpoilerGuards();
    void fixAiMcpServerLockdownAndFraming();
    void fixAiMcpBudgetReservationPricingAndPassageIndex();
    void fixAiMcpSseLocalModelsAndWavValidation();
    void opdsCoverProviderWiring();
    void aPlusArchMathFormatterUnitAndFuzz();
    void aPlusArchSpeechCleanerUnitAndFuzz();
    void aPlusArchSseParserFuzz();
    void aPlusArchSpeechResampleAntiAliasingAndMicroFade();
    void aPlusCoreBookSchemeCspAndAsync();
    void aPlusCoreMarksAnchoringHealingAndUndo();
    void aPlusCoreTranslatorFastPathAndPlurals();
    void aPlusCoreTokenizerAndCoverProviderGc();
    void aPlusCoreParserFuzzStress();
    void aPlusCraftBridgeAccessibilityAndReducedMotion();
    void aPlusCraftHotPathsCommandPaletteCatalogsPdfAndReader();
    void aPlusCraftReaderJsSwipeQueueAndScrollQuiet();
    void finalAuditPolishRegressions();
    void w5CoreLibNoReaderCycleAndDecoupledBookSearch();
    void w5CoreLibSchemaTelemetryAndReadOnlyOpen();
    void w5CoreLibFastEpubImportScanPerformance();
    void w5AudioSleepControllerMonotonicFadeAndChapterEnd();
    void w5AudioResponseSpeechAndFocusArbitration();
    void w5AudioSpeechPlayerConcurrencyStress();
    void w5AiMcpDagCyclesAndAudioPcm();
    void w5AiMcpDagCompanionExtraction();
    void w5AiMcpDagReadOnlyMcpAndSimdPassageIndex();
    void w5ReaderCraftPdfViewportTilingAndCache();
    void w5ReaderCraftAsyncLightboxSnipAndPeek();
    void w5ReaderCraftPdfSurfaceFocalZoomAndBaseCap();
    void w5ReaderCraftDecomposedReaderGpuFoldAndKeyboardA11y();
    void w6CmakeAotExternalizedI18nAndWebEngineFreeMcp();
    void speechPlayerLiveResetAndReentrancySafety();
    void uxPolishSleepIconSmoothDockTopEdgeAndLibraryMiniPlayer();
};
