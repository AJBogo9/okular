/*
    SPDX-FileCopyrightText: 2012 Fabio D 'Urso <fabiodurso@hotmail.it>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef _OKULAR_GENERATOR_PDF_ANNOTS_H_
#define _OKULAR_GENERATOR_PDF_ANNOTS_H_

#include <poppler-annotation.h>
#include <poppler-qt6.h>
#include <poppler-version.h>

#include <QMutex>

#include <unordered_map>

#include "core/annotations.h"

#include <functional>

extern Okular::Annotation *createAnnotationFromPopplerAnnotation(Poppler::Annotation *popplerAnnotation, const Poppler::Page &popplerPage, bool *doDelete);

class PopplerAnnotationProxy : public Okular::AnnotationProxy
{
public:
    /**
     * @param onPageModified called with the page this proxy just changed, so the
     * generator can stop rendering that page from copies of the file, which read
     * the document as it is on disk and cannot see an in-memory edit.
     */
    PopplerAnnotationProxy(Poppler::Document *doc, QMutex *userMutex, QHash<Okular::Annotation *, Poppler::Annotation *> *annotsOnOpenHash, std::function<void(int)> onPageModified = {});
    ~PopplerAnnotationProxy() override;

    bool supports(Capability capability) const override;
    void notifyAddition(Okular::Annotation *okl_ann, int page) override;
    void notifyModification(const Okular::Annotation *okl_ann, int page, bool appearanceChanged) override;
    void notifyRemoval(Okular::Annotation *okl_ann, int page) override;

private:
    Poppler::Document *ppl_doc;
    QMutex *mutex;
    std::function<void(int)> pageModifiedCallback;
    QHash<Okular::Annotation *, Poppler::Annotation *> *annotationsOnOpenHash;
    std::unordered_map<Okular::StampAnnotation *, std::unique_ptr<Poppler::AnnotationAppearance>> deletedStampsAnnotationAppearance;
};

#endif
