/*
    SPDX-FileCopyrightText: 2007 Tobias Koenig <tokoe@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef OKULAR_THREADEDGENERATOR_P_H
#define OKULAR_THREADEDGENERATOR_P_H

#include "area.h"

#include <QImage>
#include <QMutex>
#include <QSet>
#include <QThread>

#include <atomic>
#include <vector>

class QEventLoop;

#include "generator.h"
#include "page.h"

namespace Okular
{
class DocumentObserver;
class DocumentPrivate;
class FontInfo;
class Generator;
class Page;
class PixmapGenerationThread;
class PixmapRequest;
class TextPage;
class TextPageGenerationThread;
class TilesManager;

class GeneratorPrivate
{
public:
    GeneratorPrivate();

    virtual ~GeneratorPrivate();

    Q_DECLARE_PUBLIC(Generator)
    Generator *q_ptr;

    PixmapGenerationThread *pixmapGenerationThread(int slot);
    TextPageGenerationThread *textPageGenerationThread();

    void pixmapGenerationFinished(int slot);
    void textpageGenerationFinished();

    QMutex *threadsLock();

    /** How many renders may be in flight at once, clamped to something sane. */
    int renderSlots() const;
    /** Renders started or reserved but not yet harvested. */
    int busyRenders() const;
    /** Index of a slot with no thread running on it, or -1 if all are taken. */
    int freeRenderSlot();
    /** True once every in-flight render has been harvested. */
    bool allRendersIdle() const;

    /**
     * The render slot the calling thread is working on. Set by
     * PixmapGenerationThread::run() around the call into image(), so a generator
     * can find its per-slot resources without changing the image() signature.
     */
    static int currentRenderSlot();
    static void setCurrentRenderSlot(int slot);

    virtual QVariant metaData(const QString &key, const QVariant &option) const;
    virtual QImage image(PixmapRequest *);

    DocumentPrivate *m_document;
    // NOTE: the following should be a QSet< GeneratorFeature >,
    // but it is not to avoid #include'ing generator.h
    QSet<int> m_features;
    // One render thread per slot, created lazily. mSlotBusy[i] stays true from
    // the moment thread i is started until pixmapGenerationFinished(i) has
    // harvested its result, because the thread's request and image must not be
    // overwritten in between.
    std::vector<PixmapGenerationThread *> mPixmapGenerationThreads;
    std::vector<bool> mSlotBusy;
    int mRunningRenders;
    // Requests holding a slot but not yet started, because generatePixmap had to
    // back off and retry while the text page thread settled.
    QSet<PixmapRequest *> mReservedRequests;
    TextPageGenerationThread *mTextPageGenerationThread;
    mutable QMutex m_mutex;
    QMutex m_threadsMutex;
    bool mTextPageReady : 1;
    bool m_closing : 1;
    QEventLoop *m_closingLoop;
    QSizeF m_dpi;
    Okular::CertificateInfo::Backend m_signatureBackend = Okular::CertificateInfo::Backend::Unknown;
};

class PixmapRequestPrivate
{
public:
    void swap();
    TilesManager *tilesManager() const;

    static PixmapRequestPrivate *get(const PixmapRequest *req);

    DocumentObserver *mObserver;
    int mPageNumber;
    int mWidth;
    int mHeight;
    int mPriority;
    int mFeatures;
    bool mForce : 1;
    bool mTile : 1;
    bool mPartialUpdatesWanted : 1;
    Page *mPage;
    NormalizedRect mNormalizedRect;
    QAtomicInt mShouldAbortRender;
    QImage mResultImage;
};

class TextRequestPrivate
{
public:
    static TextRequestPrivate *get(const TextRequest *req);

    Page *mPage;
    QAtomicInt mShouldAbortExtraction;
};

class PixmapGenerationThread : public QThread
{
    Q_OBJECT

public:
    explicit PixmapGenerationThread(Generator *generator, int slot);

    int slot() const;

    void startGeneration(PixmapRequest *request, bool calcBoundingBox);

    void endGeneration();

    PixmapRequest *request() const;

    QImage image() const;
    bool calcBoundingBox() const;
    NormalizedRect boundingBox() const;

protected:
    void run() override;

private:
    Generator *mGenerator;
    PixmapRequest *mRequest;
    NormalizedRect mBoundingBox;
    int mSlot;
    bool mCalcBoundingBox : 1;
};

class TextPageGenerationThread : public QThread
{
    Q_OBJECT

public:
    explicit TextPageGenerationThread(Generator *generator);

    void endGeneration();

    void setPage(Page *page);
    Page *page() const;

    TextPage *textPage() const;

    void abortExtraction();
    bool shouldAbortExtraction() const;

public Q_SLOTS:
    void startGeneration();

protected:
    void run() override;

private:
    Generator *mGenerator;
    TextPage *mTextPage;
    TextRequest mTextRequest;
};

class FontExtractionThread : public QThread
{
    Q_OBJECT

public:
    FontExtractionThread(Generator *generator, int pages);

    void startExtraction(bool async);
    void stopExtraction();

Q_SIGNALS:
    void gotFont(const Okular::FontInfo &);
    void progress(int page);

protected:
    void run() override;

private:
    Generator *mGenerator;
    int mNumOfPages;
    std::atomic<bool> mGoOn;
};

}

Q_DECLARE_METATYPE(Okular::Page *)

#endif
