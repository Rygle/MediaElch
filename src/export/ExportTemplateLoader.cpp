#include "ExportTemplateLoader.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QNetworkReply>
#include <QXmlStreamReader>

#include "quazip/quazip.h"
#include "quazip/quazipfile.h"

#include "globals/VersionInfo.h"
#include "log/Log.h"
#include "network/NetworkRequest.h"
#include "settings/Settings.h"

static constexpr const char* s_themeListUrl =
    "https://raw.githubusercontent.com/mediaelch/mediaelch-meta/master/export_themes.xml";

ExportTemplateLoader::ExportTemplateLoader(QObject* parent) : QObject(parent)
{
    loadLocalTemplates();
}

ExportTemplateLoader* ExportTemplateLoader::instance(QObject* parent)
{
    static auto* s_instance = new ExportTemplateLoader(parent);
    return s_instance;
}

void ExportTemplateLoader::getRemoteTemplates()
{
    qCInfo(generic) << "[ExportTemplateLoader] Loading themes list from" << s_themeListUrl;
    QNetworkReply* reply = m_network.get(mediaelch::network::requestWithDefaults(QUrl(s_themeListUrl)));
    connect(reply, &QNetworkReply::finished, this, &ExportTemplateLoader::onLoadRemoteTemplatesFinished);
}

void ExportTemplateLoader::onLoadRemoteTemplatesFinished()
{
    auto* reply = dynamic_cast<QNetworkReply*>(sender());
    if (reply == nullptr) {
        return;
    }
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        qCWarning(generic) << "[ExportTemplateLoader] Network Error" << reply->errorString();
        emit sigTemplatesLoaded(mergeTemplates(m_localTemplates, m_remoteTemplates));
        return;
    }

    QString msg = QString::fromUtf8(reply->readAll());
    QXmlStreamReader xml(msg);

    if (!xml.readNextStartElement() || xml.name() != QLatin1String("themes")) {
        qCWarning(generic) << "[ExportTemplateLoader] export_themes.xml does not have a root <themes> element";
        emit sigTemplatesLoaded(mergeTemplates(m_localTemplates, m_remoteTemplates));
        return;
    }

    QVector<ExportTemplate*> templates;
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("theme")) {
            templates << parseTemplate(xml);
        } else {
            qCWarning(generic) << "[ExportTemplateLoader] Found unknown XML tag in theme list:" << xml.name();
            xml.skipCurrentElement();
        }
    }

    m_remoteTemplates = std::move(templates);

    emit sigTemplatesLoaded(mergeTemplates(m_localTemplates, m_remoteTemplates));
}

void ExportTemplateLoader::loadLocalTemplates()
{
    mediaelch::DirectoryPath location = Settings::instance()->exportTemplatesDir();
    QDir storageDir(location.dir());
    if (!storageDir.exists() && !storageDir.mkpath(location.toString())) {
        qCCritical(generic) << "[ExportTemplateLoader] Could not create storage location";
        return;
    }

    m_localTemplates.clear();
    for (QFileInfo& info : storageDir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllDirs)) {
        QList<QFileInfo> infos = QDir(info.absoluteFilePath()).entryInfoList({"metadata.xml"});
        if (infos.isEmpty() || infos.count() > 1) {
            continue;
        }

        QFile file(infos.first().absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCWarning(generic) << "[ExportTemplateLoader] File" << infos.first().absoluteFilePath()
                               << "could not be opened for reading";
            continue;
        }

        QString content = QString::fromUtf8(file.readAll());
        file.close();

        QXmlStreamReader xml(content);

        if (!xml.readNextStartElement()) {
            qCWarning(generic) << "[ExportTemplateLoader] Couldn't read XML root element of local template";
            continue;
        }

        ExportTemplate* exportTemplate = parseTemplate(xml);
        exportTemplate->setInstalled(true);
        m_localTemplates << exportTemplate;
    }
}

ExportTemplate* ExportTemplateLoader::parseTemplate(QXmlStreamReader& xml)
{
    return mediaelch::exports::parseTemplate(xml, this);
}


bool ExportTemplateLoader::validateChecksum(const QByteArray& data, const ExportTemplate& exportTemplate)
{
    if (exportTemplate.remoteFileChecksum().isEmpty()) {
        qCWarning(generic) << "[ExportTemplateLoader] No checksum found for template" << exportTemplate.name();
        return true; // TODO: Expect there to be a checksum
    }

    // Need hex representation
    QString epected = exportTemplate.remoteFileChecksum().toLower();
    QByteArray actual = QCryptographicHash::hash(data, QCryptographicHash::Algorithm::Sha256).toHex();
    if (epected != actual) {
        qCWarning(generic) << "[ExportTemplateLoader] SHA256 check fail for template" << exportTemplate.name()
                           << " | Expected:" << epected << "but found:" << actual;
        return false;
    }

    qCInfo(generic) << "[ExportTemplateLoader] SHA256 check was successful for template:" << exportTemplate.name()
                    << "with checksum:" << actual;

    return true;
}

void ExportTemplateLoader::installTemplate(ExportTemplate* exportTemplate)
{
    qCInfo(generic) << "[ExportTemplateLoader] Downloading theme" << exportTemplate->name() << "from"
                    << exportTemplate->remoteFile();
    QNetworkReply* reply = m_network.get(mediaelch::network::requestWithDefaults(QUrl(exportTemplate->remoteFile())));
    reply->setProperty("storage", QVariant::fromValue(exportTemplate));
    connect(reply, &QNetworkReply::finished, this, &ExportTemplateLoader::onDownloadTemplateFinished);
}

void ExportTemplateLoader::onDownloadTemplateFinished()
{
    auto* reply = dynamic_cast<QNetworkReply*>(sender());
    if (reply == nullptr) {
        return;
    }
    ExportTemplate* exportTemplate = reply->property("storage").value<ExportTemplate*>();
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        qCWarning(generic) << "[ExportTemplateLoader] Network Error" << reply->errorString();
        emit sigTemplateInstalled(exportTemplate, false);
        return;
    }

    QByteArray ba = reply->readAll();
    if (!validateChecksum(ba, *exportTemplate)) {
        emit sigTemplateInstalled(exportTemplate, false);
        return;
    }

    QBuffer buffer(&ba);
    if (!unpackTemplate(buffer, exportTemplate)) {
        qCDebug(generic) << "[ExportTemplateLoader] Could not unpack template";
        emit sigTemplateInstalled(exportTemplate, false);
        return;
    }

    emit sigTemplateInstalled(exportTemplate, true);

    loadLocalTemplates();
    emit sigTemplatesLoaded(mergeTemplates(m_localTemplates, m_remoteTemplates));
}

bool ExportTemplateLoader::uninstallTemplate(ExportTemplate* exportTemplate)
{
    mediaelch::DirectoryPath location = Settings::instance()->exportTemplatesDir().subDir(exportTemplate->identifier());
    QDir storageDir(location.dir());
    if (storageDir.exists() && !removeDir(storageDir.absolutePath())) {
        emit sigTemplateUninstalled(exportTemplate, false);
        return false;
    }

    loadLocalTemplates();
    emit sigTemplateUninstalled(exportTemplate, true);
    emit sigTemplatesLoaded(mergeTemplates(m_localTemplates, m_remoteTemplates));
    return true;
}


bool ExportTemplateLoader::unpackTemplate(QBuffer& buffer, ExportTemplate* exportTemplate)
{
    mediaelch::DirectoryPath location = Settings::instance()->exportTemplatesDir();
    QDir storageDir(location.dir());
    if (!storageDir.exists() && !storageDir.mkpath(location.toString())) {
        qCWarning(generic) << "[ExportTemplateLoader] Could not create storage location";
        return false;
    }

    storageDir.setPath(location.subDir(exportTemplate->identifier()).toString());
    if ((exportTemplate->isInstalled() || storageDir.exists()) && !uninstallTemplate(exportTemplate)) {
        qCWarning(generic) << "[ExportTemplateLoader] Could not uninstall template";
        return false;
    }

    if (!storageDir.mkpath(storageDir.absolutePath())) {
        qCWarning(generic) << "[ExportTemplateLoader] Could not create storage path";
        return false;
    }

    QuaZip zip(&buffer);
    if (!zip.open(QuaZip::mdUnzip)) {
        qCWarning(generic) << "[ExportTemplateLoader] Zip file could not be opened";
        return false;
    }

    if (zip.getEntriesCount() == 0) {
        qCWarning(generic) << "[ExportTemplateLoader] Zip file does not contain any entries!";
        zip.close();
        return false;
    }

    // Unpack the template.
    //
    // The old ZIP format contained all theme files _directly_.
    // Because MediaElch >2.8.0 uses GitHub for templates and their release ZIP files, there is now
    // one directory inside with the release's name.
    // We check for this format and strip the first directory name.
    const QStringList entries = zip.getFileNameList();
    const QString& baseDir = entries.first();
    const bool isGitHubReleaseFormat = std::all_of(
        entries.cbegin(), entries.cend(), [&baseDir](const QString& entry) { return entry.startsWith(baseDir); });

    if (isGitHubReleaseFormat) {
        qCInfo(generic)
            << "[ExportTemplateLoader] One directory inside ZIP. Assuming GitHub Release format. Skip first "
               "directory level.";
    }

    QuaZipFile file(&zip);
    bool hasMoreFiles = zip.goToFirstFile();
    if (hasMoreFiles && isGitHubReleaseFormat) {
        // Only one folder inside => GitHub Release Format
        // Skip the folder and "go inside".
        hasMoreFiles = zip.goToNextFile();
    }

    for (; hasMoreFiles; hasMoreFiles = zip.goToNextFile()) {
        QString filename = zip.getCurrentFileName();
        if (isGitHubReleaseFormat) {
            filename = filename.mid(baseDir.length());
        }
        if (filename.isEmpty()) {
            continue;
        }
        if (filename.endsWith("/")) {
            if (!storageDir.mkdir(filename)) {
                qCWarning(generic) << "[ExportTemplateLoader] Could not create subdirectory";
                return false;
            }
            continue;
        }
        file.open(QIODevice::ReadOnly);
        QByteArray ba = file.readAll();
        file.close();

        QFile f(QString("%1/%2").arg(storageDir.absolutePath()).arg(filename));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(ba);
            f.close();
        }
    }
    if (zip.getZipError() != UNZ_OK) {
        qCWarning(generic) << "There was an error while uncompressing the file";
        return false;
    }

    return true;
}

bool ExportTemplateLoader::removeDir(const QString& dirName)
{
    bool result = true;
    QDir dir(dirName);
    if (dir.exists(dirName)) {
        const auto entries = dir.entryInfoList(
            QDir::NoDotAndDotDot | QDir::System | QDir::Hidden | QDir::AllDirs | QDir::Files, QDir::DirsFirst);
        for (const QFileInfo& info : entries) {
            if (info.isDir()) {
                result = removeDir(info.absoluteFilePath());
            } else {
                result = QFile::remove(info.absoluteFilePath());
            }

            if (!result) {
                return false;
            }
        }
        result = dir.rmdir(dirName);
    }

    return result;
}

QVector<ExportTemplate*> ExportTemplateLoader::mergeTemplates(QVector<ExportTemplate*> local,
    QVector<ExportTemplate*> remote)
{
    QVector<ExportTemplate*> templates = local;
    for (ExportTemplate* remoteTemplate : remote) {
        bool found = false;
        for (ExportTemplate* localTemplate : templates) {
            if (localTemplate->identifier() == remoteTemplate->identifier()) {
                found = true;
                localTemplate->setRemote(true);
                localTemplate->setRemoteVersion(remoteTemplate->version());
                localTemplate->setRemoteFile(remoteTemplate->remoteFile());
            }
        }
        if (!found) {
            templates << remoteTemplate;
        }
    }

    std::sort(templates.begin(), templates.end(), ExportTemplate::lessThan);

    return templates;
}

QVector<ExportTemplate*> ExportTemplateLoader::installedTemplates()
{
    std::sort(m_localTemplates.begin(), m_localTemplates.end(), ExportTemplate::lessThan);
    return m_localTemplates;
}

ExportTemplate* ExportTemplateLoader::getTemplateByIdentifier(QString identifier)
{
    if (identifier.isEmpty()) {
        return nullptr;
    }
    auto result = std::find_if(m_localTemplates.begin(), m_localTemplates.end(), [&identifier](auto& exportTemplate) {
        return (exportTemplate->identifier() == identifier);
    });
    return (result != m_localTemplates.cend()) ? *result : nullptr;
}


namespace mediaelch {
namespace exports {

ExportTemplate* parseTemplate(QXmlStreamReader& xml, QObject* parent)
{
    // TODO: Error reporting
    auto* exportTemplate = new ExportTemplate(parent);
    exportTemplate->setRemote(true);

    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("metadata")) {
            // Do nothing; if we're here, the root element may not have been skipped, yet.
        } else if (xml.name() == QLatin1String("name")) {
            exportTemplate->setName(xml.readElementText().trimmed());
        } else if (xml.name() == QLatin1String("identifier")) {
            exportTemplate->setIdentifier(xml.readElementText().trimmed());
        } else if (xml.name() == QLatin1String("website")) {
            exportTemplate->setWebsite(xml.readElementText().trimmed());
        } else if (xml.name() == QLatin1String("description")) {
            const QString lang = xml.attributes().value("lang").toString();
            exportTemplate->addDescription(lang, xml.readElementText());
        } else if (xml.name() == QLatin1String("author")) {
            exportTemplate->addAuthor(xml.readElementText().trimmed());

        } else if (xml.name() == QLatin1String("engine")) {
            // \since v2.6.3
            QString engine = xml.readElementText();
            Q_UNUSED(engine)
            // if (engine == "simple") {
            //    exportTemplate->setTemplateEngine(ExportEngine::Simple);
            // } else {
            // default for backwards compatibility because older templates don't have a <engine> tag
            exportTemplate->setTemplateEngine(ExportEngine::Simple);
            // }

        } else if (xml.name() == QLatin1String("mediaelch-min")) {
            // \since v2.6.3
            exportTemplate->setMediaElchVersionMin(mediaelch::VersionInfo(xml.readElementText()));

        } else if (xml.name() == QLatin1String("mediaelch-max")) {
            // \since v2.6.3
            exportTemplate->setMediaElchVersionMax(mediaelch::VersionInfo(xml.readElementText()));

        } else if (xml.name() == QLatin1String("file")) {
            exportTemplate->setRemoteFile(xml.readElementText().trimmed());
        } else if (xml.name() == QLatin1String("checksum")) {
            if (xml.attributes().value("format") != QLatin1String("sha256")) {
                // Assume name is set first. If not, its just an empty string.
                qCWarning(generic) << "[ExportTemplateLoader] Unsupported checksum type; default to sha256 for"
                                   << exportTemplate->name();
            }
            exportTemplate->setRemoteFileChecksum(xml.readElementText().trimmed());
        } else if (xml.name() == QLatin1String("version")) {
            exportTemplate->setVersion(xml.readElementText());
        } else if (xml.name() == QLatin1String("supports")) {
            QVector<ExportTemplate::ExportSection> sections;
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("section")) {
                    QString section = xml.readElementText();
                    if (section == "movies") {
                        sections << ExportTemplate::ExportSection::Movies;
                    } else if (section == "tvshows") {
                        sections << ExportTemplate::ExportSection::TvShows;
                    } else if (section == "concerts") {
                        sections << ExportTemplate::ExportSection::Concerts;
                    }
                } else {
                    xml.skipCurrentElement();
                }
            }
            exportTemplate->setExportSections(sections);
        } else {
            xml.skipCurrentElement();
        }
    }

    return exportTemplate;
}
} // namespace exports
} // namespace mediaelch
