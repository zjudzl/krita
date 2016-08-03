/*
 *  Copyright (c) 2016 Boudewijn Rempt <boud@valdyas.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "DlgAnimationRenderer.h"

#include <QStandardPaths>
#include <QPluginLoader>
#include <QJsonObject>
#include <QMessageBox>
#include <QStringList>

#include <klocalizedstring.h>
#include <kpluginfactory.h>

#include <kis_properties_configuration.h>
#include <kis_debug.h>
#include <KisMimeDatabase.h>
#include <KoJsonTrader.h>
#include <KisImportExportFilter.h>
#include <kis_image.h>
#include <kis_image_animation_interface.h>
#include <kis_time_range.h>
#include <KisImportExportManager.h>
#include <kis_config_widget.h>
#include <KisDocument.h>
#include <QHBoxLayout>
#include <KisImportExportFilter.h>
#include <kis_config.h>
#include <kis_file_name_requester.h>
#include <KisDocument.h>

DlgAnimationRenderer::DlgAnimationRenderer(KisDocument *doc, QWidget *parent)
    : KoDialog(parent)
    , m_image(doc->image())
    , m_defaultFileName(QFileInfo(doc->url().toLocalFile()).completeBaseName())
{
    KisConfig cfg;

    setCaption(i18n("Render Animation"));
    setButtons(Ok | Cancel);
    setDefaultButton(Ok);

    m_page = new WdgAnimaterionRenderer(this);
    m_page->layout()->setMargin(0);
    m_page->dirRequester->setMode(KoFileDialog::OpenDirectory);
    QString lastLocation = cfg.readEntry<QString>("last_sequence_export_location", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    m_page->dirRequester->setFileName(lastLocation);

    m_page->intStart->setMinimum(doc->image()->animationInterface()->fullClipRange().start());
    m_page->intStart->setMaximum(doc->image()->animationInterface()->fullClipRange().end());
    m_page->intStart->setValue(doc->image()->animationInterface()->playbackRange().start());

    m_page->intEnd->setMinimum(doc->image()->animationInterface()->fullClipRange().start());
    m_page->intEnd->setMaximum(doc->image()->animationInterface()->fullClipRange().end());
    m_page->intEnd->setValue(doc->image()->animationInterface()->playbackRange().end());

    m_sequenceConfigLayout = new QHBoxLayout(m_page->grpExportOptions);
    m_encoderConfigLayout = new QHBoxLayout(m_page->grpRenderOptions);

    QStringList mimes = KisImportExportManager::mimeFilter(KisImportExportManager::Export);
    mimes.sort();
    Q_FOREACH(const QString &mime, mimes) {
        QString description = KisMimeDatabase::descriptionForMimeType(mime);
        if (description.isEmpty()) {
            description = mime;
        }
        m_page->cmbMimetype->addItem(description, mime);
        if (mime == "image/png") {
            m_page->cmbMimetype->setCurrentIndex(m_page->cmbMimetype->count() - 1);
        }

    }

    setMainWidget(m_page);
    resize(m_page->sizeHint());

    KoJsonTrader trader;
    QList<QPluginLoader *>list = trader.query("Krita/AnimationExporter", "");
    Q_FOREACH(QPluginLoader *loader, list) {
        QJsonObject json = loader->metaData().value("MetaData").toObject();
        QStringList mimetypes = json.value("X-KDE-Export").toString().split(",");
        Q_FOREACH(const QString &mime, mimetypes) {

            KLibFactory *factory = qobject_cast<KLibFactory *>(loader->instance());
            if (!factory) {
                warnUI << loader->errorString();
                continue;
            }

            QObject* obj = factory->create<KisImportExportFilter>(0);
            if (!obj || !obj->inherits("KisImportExportFilter")) {
                delete obj;
                continue;
            }

            QSharedPointer<KisImportExportFilter>filter(static_cast<KisImportExportFilter*>(obj));
            if (!filter) {
                delete obj;
                continue;
            }

            m_renderFilters.append(filter);

            QString description = KisMimeDatabase::descriptionForMimeType(mime);
            if (description.isEmpty()) {
                description = mime;
            }
            m_page->cmbRenderType->addItem(description, mime);

        }
    }
    m_page->videoFilename->setMode(KoFileDialog::SaveFile);
    m_page->videoFilename->setStartDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));

    qDeleteAll(list);
    connect(m_page->cmbRenderType, SIGNAL(activated(int)), this, SLOT(selectRenderType(int)));
    selectRenderType(m_page->cmbRenderType->currentIndex());

    connect(m_page->grpRender, SIGNAL(toggled(bool)), this, SLOT(toggleSequenceType(bool)));
    connect(m_page->cmbMimetype, SIGNAL(activated(int)), this, SLOT(sequenceMimeTypeSelected(int)));
    sequenceMimeTypeSelected(m_page->cmbMimetype->currentIndex());

    QString ffmpeg = cfg.customFFMpegPath();
    m_page->ffmpegLocation->setFileName(ffmpeg);
    m_page->ffmpegLocation->setMode(KoFileDialog::OpenFile);
    connect(m_page->ffmpegLocation, SIGNAL(fileSelected(QString)), this, SLOT(ffmpegLocationChanged(QString)));

    m_page->grpRender->setChecked(cfg.readEntry<bool>("render_animation", false));
}

DlgAnimationRenderer::~DlgAnimationRenderer()
{
    KisConfig cfg;
    cfg.writeEntry<bool>("render_animation", m_page->grpRender->isChecked());
    cfg.writeEntry<QString>("last_sequence_export_location", m_page->dirRequester->fileName());
    cfg.setCustomFFMpegPath(m_page->ffmpegLocation->fileName());
    m_encoderConfigWidget->setParent(0);
    m_encoderConfigWidget->deleteLater();
    m_frameExportConfigWidget->setParent(0);
    m_frameExportConfigWidget->deleteLater();

    delete m_page;

}

KisPropertiesConfigurationSP DlgAnimationRenderer::getSequenceConfiguration() const
{
    KisPropertiesConfigurationSP cfg = new KisPropertiesConfiguration();
    cfg->setProperty("basename", m_page->txtBasename->text());
    cfg->setProperty("directory", m_page->dirRequester->fileName());
    cfg->setProperty("first_frame", m_page->intStart->value());
    cfg->setProperty("last_frame", m_page->intEnd->value());
    cfg->setProperty("sequence_start", m_page->sequenceStart->value());
    cfg->setProperty("mimetype", m_page->cmbMimetype->currentData().toString());
    return cfg;
}

void DlgAnimationRenderer::setSequenceConfiguration(KisPropertiesConfigurationSP cfg)
{
    m_page->txtBasename->setText(cfg->getString("basename", "frame"));
    m_page->dirRequester->setFileName(cfg->getString("directory", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)));
    m_page->intStart->setValue(cfg->getInt("first_frame", m_image->animationInterface()->playbackRange().start()));
    m_page->intEnd->setValue(cfg->getInt("last_frame", m_image->animationInterface()->playbackRange().end()));
    m_page->sequenceStart->setValue(cfg->getInt("sequence_start", m_image->animationInterface()->playbackRange().start()));
    QString mimetype = cfg->getString("mimetype");
    for (int i = 0; i < m_page->cmbMimetype->count(); ++i) {
        if (m_page->cmbMimetype->itemData(i).toString() == mimetype) {
            m_page->cmbMimetype->setCurrentIndex(i);
            sequenceMimeTypeSelected(i);
            break;
        }
    }
}

KisPropertiesConfigurationSP DlgAnimationRenderer::getFrameExportConfiguration() const
{
    if (m_frameExportConfigWidget) {
        KisPropertiesConfigurationSP cfg = m_frameExportConfigWidget->configuration();
        cfg->setProperty("basename", m_page->txtBasename->text());
        cfg->setProperty("directory", m_page->dirRequester->fileName());
        cfg->setProperty("first_frame", m_page->intStart->value());
        cfg->setProperty("last_frame", m_page->intEnd->value());
        cfg->setProperty("sequence_start", m_page->sequenceStart->value());

        return m_frameExportConfigWidget->configuration();
    }
    return 0;
}

bool DlgAnimationRenderer::renderToVideo() const
{
    return m_page->grpRender->isChecked();
}

KisPropertiesConfigurationSP DlgAnimationRenderer::getVideoConfiguration() const
{
    if (!m_page->grpRender->isChecked()) {
        return 0;
    }
    KisPropertiesConfigurationSP cfg = new KisPropertiesConfiguration();
    cfg->setProperty("filename", m_page->videoFilename->fileName());
    cfg->setProperty("delete_sequence", m_page->chkDeleteSequence->isChecked());
    return cfg;
}

void DlgAnimationRenderer::setVideoConfiguration(KisPropertiesConfigurationSP /*cfg*/)
{
}

KisPropertiesConfigurationSP DlgAnimationRenderer::getEncoderConfiguration() const
{
    if (!m_page->grpRender->isChecked()) {
        return 0;
    }
    KisPropertiesConfigurationSP cfg = new KisPropertiesConfiguration();

    cfg->setProperty("mimetype", m_page->cmbRenderType->currentData().toString());
    return cfg;
}

void DlgAnimationRenderer::setEncoderConfiguration(KisPropertiesConfigurationSP /*cfg*/)
{

}

QSharedPointer<KisImportExportFilter> DlgAnimationRenderer::encoderFilter() const
{
    if (m_page->cmbRenderType->currentIndex() < m_renderFilters.size()) {
        return m_renderFilters[m_page->cmbRenderType->currentIndex()];
    }
    return QSharedPointer<KisImportExportFilter>(0);
}

void DlgAnimationRenderer::selectRenderType(int index)
{
    if (m_encoderConfigWidget) {
        m_encoderConfigLayout->removeWidget(m_encoderConfigWidget);
        m_encoderConfigWidget->hide();
        m_encoderConfigWidget->setParent(0);
        m_encoderConfigWidget->deleteLater();
        m_encoderConfigWidget = 0;
    }

    if (index >= m_renderFilters.size()) return;

    QSharedPointer<KisImportExportFilter> filter = m_renderFilters[index];
    QString mimetype = m_page->cmbRenderType->itemData(index).toString();

    if (!m_page->videoFilename->fileName().isEmpty() && QFileInfo(m_page->videoFilename->fileName()).completeBaseName() != m_defaultFileName) {
        m_defaultFileName = QFileInfo(m_page->videoFilename->fileName()).completeBaseName();
    }
    m_page->videoFilename->setMimeTypeFilters(QStringList() << mimetype, mimetype);
    m_page->videoFilename->setFileName(m_defaultFileName + "." + KisMimeDatabase::suffixesForMimeType(mimetype).first());

    if (filter) {
        m_encoderConfigWidget = filter->createConfigurationWidget(m_page->grpExportOptions, KisDocument::nativeFormatMimeType(), mimetype.toLatin1());
        if (m_encoderConfigWidget) {
            m_encoderConfigLayout->addWidget(m_encoderConfigWidget);
            m_encoderConfigWidget->setConfiguration(filter->lastSavedConfiguration());
            m_encoderConfigWidget->show();
            resize(sizeHint());
        }
        else {
            m_encoderConfigWidget = 0;
        }
    }
}

void DlgAnimationRenderer::toggleSequenceType(bool toggle)
{
    m_page->cmbMimetype->setEnabled(!toggle);
    for (int i = 0; i < m_page->cmbMimetype->count(); ++i) {
        if (m_page->cmbMimetype->itemData(i).toString() == "image/png") {
            m_page->cmbMimetype->setCurrentIndex(i);
            break;
        }
    }
}

void DlgAnimationRenderer::sequenceMimeTypeSelected(int index)
{
    if (m_frameExportConfigWidget) {
        m_sequenceConfigLayout->removeWidget(m_frameExportConfigWidget);
        m_frameExportConfigWidget->hide();
        m_frameExportConfigWidget->setParent(0);
        m_frameExportConfigWidget->deleteLater();
        m_frameExportConfigWidget = 0;
    }
    QString mimetype = m_page->cmbMimetype->itemData(index).toString();
    KisImportExportFilter *filter = KisImportExportManager::filterForMimeType(mimetype, KisImportExportManager::Export);
    if (filter) {
        m_frameExportConfigWidget = filter->createConfigurationWidget(m_page->grpExportOptions, KisDocument::nativeFormatMimeType(), mimetype.toLatin1());
        if (m_frameExportConfigWidget) {
            m_sequenceConfigLayout->addWidget(m_frameExportConfigWidget);
            m_frameExportConfigWidget->setConfiguration(filter->lastSavedConfiguration());
            m_frameExportConfigWidget->show();
            resize(sizeHint());
        }
        else {
            m_frameExportConfigWidget = 0;
        }
        delete filter;
    }
}

void DlgAnimationRenderer::ffmpegLocationChanged(const QString &s)
{
    KisConfig cfg;
    cfg.setCustomFFMpegPath(s);
}

void DlgAnimationRenderer::slotButtonClicked(int button)
{
    if (button == KoDialog::Ok && m_page->grpRender->isChecked()) {
        QString ffmpeg = m_page->ffmpegLocation->fileName();
        if (m_page->videoFilename->fileName().isEmpty()) {
            QMessageBox::warning(this, i18nc("@title:window", "Krita"), i18n("Please enter a file name to render to."));
            return;
        }
        else if (ffmpeg.isEmpty()) {
            QMessageBox::warning(this, i18nc("@title:window", "Krita"), i18n("The location of FFmpeg is unknown. Please install FFmpeg first: Krita cannot render animations without FFmpeg. (<a href=\"https://www.ffmpeg.org\">www.ffmpeg.org</a>)"));
            return;
        }
        else {
            QFileInfo fi(ffmpeg);
            if (!fi.exists()) {
                QMessageBox::warning(this, i18nc("@title:window", "Krita"), i18n("The location of FFmpeg is invalid. Please select the correct location of the FFmpeg executable on your system."));
                return;
            }
        }
    }
    KoDialog::slotButtonClicked(button);
}

