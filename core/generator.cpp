/*
    SPDX-FileCopyrightText: 2005 Piotr Szymanski <niedakh@gmail.com>
    SPDX-FileCopyrightText: 2008 Albert Astals Cid <aacid@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "config-okular.h"

#include "generator.h"
#include "generator_p.h"
#include "observer.h"

#include <QApplication>
#include <QEventLoop>
#include <QPrinter>

#include <KLocalizedString>
#include <QDebug>
#include <QIcon>
#include <QMimeDatabase>
#include <QTimer>

#if HAVE_KWALLET
#include <KWallet>
#endif

#include "document_p.h"
#include "page.h"
#include "page_p.h"
#include "textpage.h"
#include "utils.h"

using namespace Okular;

GeneratorPrivate::GeneratorPrivate()
    : q_ptr(nullptr)
    , m_document(nullptr)
    , mRunningRenders(0)
    , mTextPageGenerationThread(nullptr)
    , mTextPageReady(true)
    , m_closing(false)
    , m_closingLoop(nullptr)
    , m_dpi(72.0, 72.0)
{
    qRegisterMetaType<Okular::Page *>();
}

GeneratorPrivate::~GeneratorPrivate()
{
    for (PixmapGenerationThread *thread : mPixmapGenerationThreads) {
        if (thread) {
            thread->wait();
            delete thread;
        }
    }
    mPixmapGenerationThreads.clear();

    if (mTextPageGenerationThread) {
        mTextPageGenerationThread->wait();
    }

    delete mTextPageGenerationThread;
}

namespace
{
// Which render slot the current thread is working on. Read by
// Generator::currentRenderSlot(); 0 on the main thread and on any generator that
// never raised maxConcurrentRenders().
thread_local int s_currentRenderSlot = 0;
}

int GeneratorPrivate::currentRenderSlot()
{
    return s_currentRenderSlot;
}

void GeneratorPrivate::setCurrentRenderSlot(int slot)
{
    s_currentRenderSlot = slot;
}

int GeneratorPrivate::renderSlots() const
{
    // Asked on every dispatch rather than cached, so a generator that has to
    // drop back to serial rendering mid-document (a PDF gains an annotation, say)
    // takes effect as soon as the renders already in flight have drained.
    return qBound(1, q_ptr->maxConcurrentRenders(), 64);
}

int GeneratorPrivate::busyRenders() const
{
    return mRunningRenders + int(mReservedRequests.size());
}

bool GeneratorPrivate::allRendersIdle() const
{
    return mRunningRenders == 0 && mReservedRequests.isEmpty();
}

int GeneratorPrivate::freeRenderSlot()
{
    const int slots = renderSlots();
    if (int(mSlotBusy.size()) < slots) {
        mSlotBusy.resize(slots, false);
    }
    for (int i = 0; i < slots; ++i) {
        if (!mSlotBusy[i]) {
            return i;
        }
    }
    return -1;
}

PixmapGenerationThread *GeneratorPrivate::pixmapGenerationThread(int slot)
{
    if (slot < 0) {
        slot = 0;
    }
    if (int(mPixmapGenerationThreads.size()) <= slot) {
        mPixmapGenerationThreads.resize(slot + 1, nullptr);
    }
    if (mPixmapGenerationThreads[slot]) {
        return mPixmapGenerationThreads[slot];
    }

    Q_Q(Generator);
    PixmapGenerationThread *thread = new PixmapGenerationThread(q, slot);
    mPixmapGenerationThreads[slot] = thread;
    QObject::connect(thread, &PixmapGenerationThread::finished, q, [this, slot] { pixmapGenerationFinished(slot); }, Qt::QueuedConnection);

    return thread;
}

TextPageGenerationThread *GeneratorPrivate::textPageGenerationThread()
{
    if (mTextPageGenerationThread) {
        return mTextPageGenerationThread;
    }

    Q_Q(Generator);
    mTextPageGenerationThread = new TextPageGenerationThread(q);
    QObject::connect(mTextPageGenerationThread, &TextPageGenerationThread::finished, q, [this] { textpageGenerationFinished(); }, Qt::QueuedConnection);

    return mTextPageGenerationThread;
}

void GeneratorPrivate::pixmapGenerationFinished(int slot)
{
    Q_Q(Generator);
    PixmapGenerationThread *thread = mPixmapGenerationThreads.at(slot);
    PixmapRequest *request = thread->request();
    const QImage &img = thread->image();
    const bool wantsBoundingBox = thread->calcBoundingBox();
    const NormalizedRect boundingBox = thread->boundingBox();
    thread->endGeneration();

    QMutexLocker locker(threadsLock());

    // Only now, with the request and image harvested, may this slot be handed to
    // another request.
    mSlotBusy[slot] = false;
    --mRunningRenders;

    if (m_closing) {
        delete request;
        if (allRendersIdle() && mTextPageReady) {
            locker.unlock();
            m_closingLoop->quit();
        }
        return;
    }

    if (!request->shouldAbortRender()) {
        request->page()->setPixmap(request->observer(), new QPixmap(QPixmap::fromImage(img)), request->normalizedRect());
        const int pageNumber = request->page()->number();

        if (wantsBoundingBox) {
            q->updatePageBoundingBox(pageNumber, boundingBox);
        }
    } else {
        // Cancel the text page generation too if it's still running
        if (mTextPageGenerationThread && mTextPageGenerationThread->isRunning()) {
            mTextPageGenerationThread->abortExtraction();
            mTextPageGenerationThread->wait();
        }
    }

    q->signalPixmapRequestDone(request);
}

void GeneratorPrivate::textpageGenerationFinished()
{
    Q_Q(Generator);
    Page *page = mTextPageGenerationThread->page();
    mTextPageGenerationThread->endGeneration();

    QMutexLocker locker(threadsLock());
    mTextPageReady = true;

    if (m_closing) {
        delete mTextPageGenerationThread->textPage();
        if (allRendersIdle()) {
            locker.unlock();
            m_closingLoop->quit();
        }
        return;
    }

    if (mTextPageGenerationThread->textPage()) {
        TextPage *tp = mTextPageGenerationThread->textPage();
        page->setTextPage(tp);
        q->signalTextGenerationDone(page, tp);
    }
}

QMutex *GeneratorPrivate::threadsLock()
{
    return &m_threadsMutex;
}

QVariant GeneratorPrivate::metaData(const QString &, const QVariant &) const
{
    return QVariant();
}

QImage GeneratorPrivate::image(PixmapRequest *)
{
    return QImage();
}

Generator::Generator(QObject *parent, const QVariantList &args)
    : Generator(*new GeneratorPrivate(), parent, args)
{
    // the delegated constructor does it all
}

Generator::Generator(GeneratorPrivate &dd, QObject *parent, const QVariantList &args)
    : QObject(parent)
    , d_ptr(&dd)
{
    d_ptr->q_ptr = this;
    Q_UNUSED(args)
}

Generator::~Generator()
{
    delete d_ptr;
}

bool Generator::loadDocument(const QString &fileName, QList<Page *> &pagesVector)
{
    Q_UNUSED(fileName);
    Q_UNUSED(pagesVector);

    return false;
}

bool Generator::loadDocumentFromData(const QByteArray &, QList<Page *> &)
{
    return false;
}

Document::OpenResult Generator::loadDocumentWithPassword(const QString &fileName, QList<Page *> &pagesVector, const QString &)
{
    return loadDocument(fileName, pagesVector) ? Document::OpenSuccess : Document::OpenError;
}

Document::OpenResult Generator::loadDocumentFromDataWithPassword(const QByteArray &fileData, QList<Page *> &pagesVector, const QString &)
{
    return loadDocumentFromData(fileData, pagesVector) ? Document::OpenSuccess : Document::OpenError;
}

Generator::SwapBackingFileResult Generator::swapBackingFile(QString const & /*newFileName */, QList<Okular::Page *> & /*newPagesVector*/)
{
    return SwapBackingFileError;
}

bool Generator::closeDocument()
{
    Q_D(Generator);

    d->m_closing = true;

    d->threadsLock()->lock();
    if (!(d->allRendersIdle() && d->mTextPageReady)) {
        QEventLoop loop;
        d->m_closingLoop = &loop;

        d->threadsLock()->unlock();

        loop.exec();

        d->m_closingLoop = nullptr;
    } else {
        d->threadsLock()->unlock();
    }

    bool ret = doCloseDocument();

    d->m_closing = false;

    return ret;
}

bool Generator::canGeneratePixmap() const
{
    Q_D(const Generator);
    return d->busyRenders() < d->renderSlots();
}

int Generator::maxConcurrentRenders() const
{
    return 1;
}

int Generator::currentRenderSlot() const
{
    return GeneratorPrivate::currentRenderSlot();
}

bool Generator::canSign() const
{
    return false;
}

std::pair<SigningResult, QString> Generator::sign(const NewSignatureData &, const QString &)
{
    return {};
}

CertificateStore *Generator::certificateStore() const
{
    return nullptr;
}

Okular::CertificateInfo::Backend Generator::activeCertificateBackend() const
{
    Q_D(const Generator);
    return d->m_signatureBackend;
}

void Generator::setActiveCertificateBackend(Okular::CertificateInfo::Backend newBackend)
{
    Q_D(Generator);
    d->m_signatureBackend = newBackend;
}

void Generator::generatePixmap(PixmapRequest *request)
{
    Q_D(Generator);

    const bool calcBoundingBox = !request->isTile() && !request->page()->isBoundingBoxKnown();

    if (request->asynchronous() && hasFeature(Threaded)) {
        // Claim a slot for this request up front and hold it across any retry
        // below, so the core does not keep dispatching into a generator that is
        // really already full. The claim is converted into a running render, or
        // dropped, before this function returns.
        d->mReservedRequests.insert(request);

        if (d->textPageGenerationThread()->isFinished() && !canGenerateTextPage()) {
            // It can happen that the text generation has already finished but
            // mTextPageReady is still false because textpageGenerationFinished
            // didn't have time to run, if so queue ourselves
            QTimer::singleShot(0, this, [this, request] { generatePixmap(request); });
            return;
        }

        const int slot = d->freeRenderSlot();
        if (slot < 0) {
            // Every render thread is still busy; come back when one is harvested.
            QTimer::singleShot(0, this, [this, request] { generatePixmap(request); });
            return;
        }
        PixmapGenerationThread *thread = d->pixmapGenerationThread(slot);

        /**
         * We create the text page for every page that is visible to the
         * user, so he can use the text extraction tools without a delay.
         */
        if (hasFeature(TextExtraction) && !request->page()->hasTextPage() && canGenerateTextPage() && !d->m_closing) {
            d->mTextPageReady = false;
            d->textPageGenerationThread()->setPage(request->page());

            // dummy is used as a way to make sure the lambda gets disconnected each time it is executed
            // since not all the times the pixmap generation thread starts we want the text generation thread to also start
            QObject *dummy = new QObject();
            connect(thread, &QThread::started, dummy, [this, dummy] {
                delete dummy;
                d_ptr->textPageGenerationThread()->startGeneration();
            });
        }

        // Turn the reservation into a running render before starting the thread:
        // the slot must already read as busy when the completion lands.
        d->mReservedRequests.remove(request);
        d->mSlotBusy[slot] = true;
        ++d->mRunningRenders;

        // pixmap generation thread must be started *after* connect(), else we may miss the start signal and get lock-ups (see bug 396137)
        thread->startGeneration(request, calcBoundingBox);

        return;
    }

    // Synchronous rendering happens on the calling thread, which owns slot 0.
    GeneratorPrivate::setCurrentRenderSlot(0);
    const QImage &img = image(request);
    request->page()->setPixmap(request->observer(), new QPixmap(QPixmap::fromImage(img)), request->normalizedRect());
    const int pageNumber = request->page()->number();

    signalPixmapRequestDone(request);
    if (calcBoundingBox) {
        updatePageBoundingBox(pageNumber, Utils::imageBoundingBox(&img));
    }
}

bool Generator::canGenerateTextPage() const
{
    Q_D(const Generator);
    return d->mTextPageReady;
}

void Generator::generateTextPage(Page *page)
{
    TextRequest treq(page);
    TextPage *tp = textPage(&treq);
    page->setTextPage(tp);
    signalTextGenerationDone(page, tp);
}

QImage Generator::image(PixmapRequest *request)
{
    Q_D(Generator);
    return d->image(request);
}

TextPage *Generator::textPage(TextRequest *)
{
    return nullptr;
}

DocumentInfo Generator::generateDocumentInfo(const QSet<DocumentInfo::Key> &keys) const
{
    Q_UNUSED(keys);

    return DocumentInfo();
}

const DocumentSynopsis *Generator::generateDocumentSynopsis()
{
    return nullptr;
}

FontInfo::List Generator::fontsForPage(int)
{
    return FontInfo::List();
}

const QList<EmbeddedFile *> *Generator::embeddedFiles() const
{
    return nullptr;
}

Generator::PageLayout Generator::defaultPageLayout() const
{
    return NoLayout;
}

bool Generator::defaultPageContinuous() const
{
    return false;
}

Generator::PageSizeMetric Generator::pagesSizeMetric() const
{
    return None;
}

bool Generator::isAllowed(Permission) const
{
    return true;
}

void Generator::rotationChanged(Rotation, Rotation)
{
}

PageSize::List Generator::pageSizes() const
{
    return PageSize::List();
}

void Generator::pageSizeChanged(const PageSize &, const PageSize &)
{
}

Document::PrintError Generator::print(QPrinter &)
{
    return Document::UnknownPrintError;
}

BackendOpaqueAction::OpaqueActionResult Generator::opaqueAction(const BackendOpaqueAction * /*action*/)
{
    return BackendOpaqueAction::DoNothing;
}

void Generator::freeOpaqueActionContents(const BackendOpaqueAction & /*action*/)
{
}

QVariant Generator::metaData(const QString &key, const QVariant &option) const
{
    Q_D(const Generator);
    return d->metaData(key, option);
}

ExportFormat::List Generator::exportFormats() const
{
    return ExportFormat::List();
}

bool Generator::exportTo(const QString &, const ExportFormat &)
{
    return false;
}

void Generator::walletDataForFile(const QString &fileName, QString *walletName, QString *walletFolder, QString *walletKey) const
{
#if HAVE_KWALLET
    *walletKey = fileName.section(QLatin1Char('/'), -1, -1);
    *walletName = KWallet::Wallet::NetworkWallet();
    *walletFolder = QStringLiteral("KPdf");
#else
    Q_UNUSED(fileName);
    Q_UNUSED(walletName);
    Q_UNUSED(walletFolder);
    Q_UNUSED(walletKey);
#endif
}

bool Generator::hasFeature(GeneratorFeature feature) const
{
    Q_D(const Generator);
    return d->m_features.contains(feature);
}

void Generator::signalPixmapRequestDone(PixmapRequest *request)
{
    Q_D(Generator);
    if (d->m_document) {
        d->m_document->requestDone(request);
    } else {
        delete request;
    }
}

void Generator::signalTextGenerationDone(Page *page, TextPage *textPage)
{
    Q_D(Generator);
    if (d->m_document) {
        d->m_document->textGenerationDone(page);
    } else {
        delete textPage;
    }
}

void Generator::signalPartialPixmapRequest(PixmapRequest *request, const QImage &image)
{
    if (request->shouldAbortRender()) {
        return;
    }

    PagePrivate *pagePrivate = PagePrivate::get(request->page());
    pagePrivate->setPixmap(request->observer(), new QPixmap(QPixmap::fromImage(image)), request->normalizedRect(), true /* isPartialPixmap */);

    const int pageNumber = request->page()->number();
    request->observer()->notifyPageChanged(pageNumber, Okular::DocumentObserver::Pixmap);
}

const Document *Generator::document() const
{
    Q_D(const Generator);
    if (d->m_document) {
        return d->m_document->m_parent;
    }
    return nullptr;
}

Okular::Action *Generator::additionalDocumentAction(Okular::Document::DocumentAdditionalActionType)
{
    return nullptr;
}

void Generator::setFeature(GeneratorFeature feature, bool on)
{
    Q_D(Generator);
    if (on) {
        d->m_features.insert(feature);
    } else {
        d->m_features.remove(feature);
    }
}

QVariant Generator::documentMetaData(const DocumentMetaDataKey key, const QVariant &option) const
{
    Q_D(const Generator);
    if (!d->m_document) {
        return QVariant();
    }

    return d->m_document->documentMetaData(key, option);
}

QMutex *Generator::userMutex() const
{
    Q_D(const Generator);
    return &d->m_mutex;
}

void Generator::updatePageBoundingBox(int page, const NormalizedRect &boundingBox)
{
    Q_D(Generator);
    if (d->m_document) { // still connected to document?
        d->m_document->setPageBoundingBox(page, boundingBox);
    }
}

QByteArray Generator::requestFontData(const Okular::FontInfo & /*font*/)
{
    return {};
}

void Generator::setDPI(const QSizeF dpi)
{
    Q_D(Generator);
    d->m_dpi = dpi;
}

QSizeF Generator::dpi() const
{
    Q_D(const Generator);
    return d->m_dpi;
}

QAbstractItemModel *Generator::layersModel() const
{
    return nullptr;
}

TextRequest::TextRequest()
    : d(new TextRequestPrivate)
{
    d->mPage = nullptr;
    d->mShouldAbortExtraction = 0;
}

TextRequest::TextRequest(Page *page)
    : d(new TextRequestPrivate)
{
    d->mPage = page;
    d->mShouldAbortExtraction = 0;
}

TextRequest::~TextRequest()
{
    delete d;
}

Page *TextRequest::page() const
{
    return d->mPage;
}

bool TextRequest::shouldAbortExtraction() const
{
    return d->mShouldAbortExtraction != 0;
}

TextRequestPrivate *TextRequestPrivate::get(const TextRequest *req)
{
    return req->d;
}

PixmapRequest::PixmapRequest(DocumentObserver *observer, int pageNumber, int width, int height, qreal dpr, int priority, PixmapRequestFeatures features)
    : d(new PixmapRequestPrivate)
{
    d->mObserver = observer;
    d->mPageNumber = pageNumber;
    d->mWidth = ceil(width * dpr);
    d->mHeight = ceil(height * dpr);
    d->mPriority = priority;
    d->mFeatures = features;
    d->mForce = false;
    d->mTile = false;
    d->mNormalizedRect = NormalizedRect();
    d->mPartialUpdatesWanted = false;
    d->mShouldAbortRender = 0;
}

PixmapRequest::~PixmapRequest()
{
    delete d;
}

DocumentObserver *PixmapRequest::observer() const
{
    return d->mObserver;
}

int PixmapRequest::pageNumber() const
{
    return d->mPageNumber;
}

int PixmapRequest::width() const
{
    return d->mWidth;
}

int PixmapRequest::height() const
{
    return d->mHeight;
}

int PixmapRequest::priority() const
{
    return d->mPriority;
}

bool PixmapRequest::asynchronous() const
{
    return d->mFeatures & Asynchronous;
}

bool PixmapRequest::preload() const
{
    return d->mFeatures & Preload;
}

Page *PixmapRequest::page() const
{
    return d->mPage;
}

void PixmapRequest::setTile(bool tile)
{
    d->mTile = tile;
}

bool PixmapRequest::isTile() const
{
    return d->mTile;
}

void PixmapRequest::setNormalizedRect(const NormalizedRect &rect)
{
    if (d->mNormalizedRect == rect) {
        return;
    }

    d->mNormalizedRect = rect;
}

const NormalizedRect &PixmapRequest::normalizedRect() const
{
    return d->mNormalizedRect;
}

void PixmapRequest::setPartialUpdatesWanted(bool partialUpdatesWanted)
{
    d->mPartialUpdatesWanted = partialUpdatesWanted;
}

bool PixmapRequest::partialUpdatesWanted() const
{
    return d->mPartialUpdatesWanted;
}

bool PixmapRequest::shouldAbortRender() const
{
    return d->mShouldAbortRender != 0;
}

Okular::TilesManager *PixmapRequestPrivate::tilesManager() const
{
    return mPage->d->tilesManager(mObserver);
}

PixmapRequestPrivate *PixmapRequestPrivate::get(const PixmapRequest *req)
{
    return req->d;
}

void PixmapRequestPrivate::swap()
{
    std::swap(mWidth, mHeight);
}

class Okular::ExportFormatPrivate : public QSharedData
{
public:
    ExportFormatPrivate(const QString &description, const QMimeType &mimeType, const QIcon &icon = QIcon())
        : QSharedData()
        , mDescription(description)
        , mMimeType(mimeType)
        , mIcon(icon)
    {
    }
    ~ExportFormatPrivate()
    {
    }

    QString mDescription;
    QMimeType mMimeType;
    QIcon mIcon;
};

ExportFormat::ExportFormat()
    : d(new ExportFormatPrivate(QString(), QMimeType()))
{
}

ExportFormat::ExportFormat(const QString &description, const QMimeType &mimeType)
    : d(new ExportFormatPrivate(description, mimeType))
{
}

ExportFormat::ExportFormat(const QIcon &icon, const QString &description, const QMimeType &mimeType)
    : d(new ExportFormatPrivate(description, mimeType, icon))
{
}

ExportFormat::~ExportFormat()
{
}

ExportFormat::ExportFormat(const ExportFormat &other)
    : d(other.d)
{
}

ExportFormat &ExportFormat::operator=(const ExportFormat &other)
{
    if (this == &other) {
        return *this;
    }

    d = other.d;

    return *this;
}

QString ExportFormat::description() const
{
    return d->mDescription;
}

QMimeType ExportFormat::mimeType() const
{
    return d->mMimeType;
}

QIcon ExportFormat::icon() const
{
    return d->mIcon;
}

bool ExportFormat::isNull() const
{
    return !d->mMimeType.isValid() || d->mDescription.isNull();
}

ExportFormat ExportFormat::standardFormat(StandardExportFormat type)
{
    QMimeDatabase db;
    switch (type) {
    case PlainText:
        return ExportFormat(QIcon::fromTheme(QStringLiteral("text-x-generic")), i18n("Plain &Text…"), db.mimeTypeForName(QStringLiteral("text/plain")));
        break;
    case PDF:
        return ExportFormat(QIcon::fromTheme(QStringLiteral("application-pdf")), i18n("PDF"), db.mimeTypeForName(QStringLiteral("application/pdf")));
        break;
    case OpenDocumentText:
        return ExportFormat(
            QIcon::fromTheme(QStringLiteral("application-vnd.oasis.opendocument.text")), i18nc("This is the document format", "OpenDocument Text"), db.mimeTypeForName(QStringLiteral("application/vnd.oasis.opendocument.text")));
        break;
    case HTML:
        return ExportFormat(QIcon::fromTheme(QStringLiteral("text-html")), i18nc("This is the document format", "HTML"), db.mimeTypeForName(QStringLiteral("text/html")));
        break;
    }
    return ExportFormat();
}

bool ExportFormat::operator==(const ExportFormat &other) const
{
    return d == other.d;
}

bool ExportFormat::operator!=(const ExportFormat &other) const
{
    return d != other.d;
}

QDebug operator<<(QDebug str, const Okular::PixmapRequest &req)
{
    PixmapRequestPrivate *reqPriv = PixmapRequestPrivate::get(&req);

    str << "PixmapRequest:" << &req;
    str << "- observer:" << (qulonglong)req.observer();
    str << "- page:" << req.pageNumber();
    str << "- width:" << req.width();
    str << "- height:" << req.height();
    str << "- priority:" << req.priority();
    str << "- async:" << (req.asynchronous() ? "true" : "false");
    str << "- tile:" << (req.isTile() ? "true" : "false");
    str << "- rect:" << req.normalizedRect();
    str << "- preload:" << (req.preload() ? "true" : "false");
    str << "- partialUpdates:" << (req.partialUpdatesWanted() ? "true" : "false");
    str << "- shouldAbort:" << (req.shouldAbortRender() ? "true" : "false");
    str << "- force:" << (reqPriv->mForce ? "true" : "false");
    return str;
}

/* kate: replace-tabs on; indent-width 4; */
