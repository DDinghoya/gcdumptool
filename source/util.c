#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <switch/services/ncm.h>
#include <switch/services/ns.h>
#include <libxml2/libxml/globals.h>
#include <libxml2/libxml/xpath.h>
#include <curl/curl.h>
#include <json-c/json.h>

#include "dumper.h"
#include "fsext.h"
#include "ui.h"
#include "util.h"

/* Extern variables */

extern int breaks;
extern int font_height;

extern int cursor;
extern int scroll;

/* Constants */

const char *nswReleasesXmlUrl = "http://nswdb.com/xml.php";
const char *nswReleasesXmlTmpPath = "sdmc:/NSWreleases.xml.tmp";
const char *nswReleasesXmlPath = "sdmc:/NSWreleases.xml";
const char *nswReleasesRootElement = "releases";
const char *nswReleasesChildren = "release";
const char *nswReleasesChildrenImageSize = "imagesize";
const char *nswReleasesChildrenTitleID = "titleid";
const char *nswReleasesChildrenImgCrc = "imgcrc";
const char *nswReleasesChildrenReleaseName = "releasename";

const char *githubReleasesApiUrl = "https://api.github.com/repos/DarkMatterCore/gcdumptool/releases/latest";
const char *gcDumpToolTmpPath = "sdmc:/switch/gcdumptool.nro.tmp";
const char *gcDumpToolPath = "sdmc:/switch/gcdumptool.nro";

const char *userAgent = "gcdumptool/" APP_VERSION " (Nintendo Switch)";

/* Statically allocated variables */

static char *result_buf = NULL;
static size_t result_sz = 0;
static size_t result_written = 0;

char *filenameBuffer = NULL;
char *filenames[FILENAME_MAX_CNT];
int filenamesCount = 0;

FsDeviceOperator fsOperatorInstance;
FsEventNotifier fsGameCardEventNotifier;
Handle fsGameCardEventHandle;
Event fsGameCardKernelEvent;
UEvent exitEvent;

AppletType programAppletType;

bool gameCardInserted;

u64 gameCardSize = 0, trimmedCardSize = 0;
char gameCardSizeStr[32] = {'\0'}, trimmedCardSizeStr[32] = {'\0'};

char *hfs0_header = NULL;
u64 hfs0_offset = 0, hfs0_size = 0;
u32 hfs0_partition_cnt = 0;

char *partitionHfs0Header = NULL;
u64 partitionHfs0HeaderOffset = 0, partitionHfs0HeaderSize = 0;
u32 partitionHfs0FileCount = 0, partitionHfs0StrTableSize = 0;

u64 gameCardTitleID = 0;
u32 gameCardVersion = 0;
char gameCardName[0x201] = {'\0'}, fixedGameCardName[0x201] = {'\0'}, gameCardAuthor[0x101] = {'\0'}, gameCardVersionStr[64] = {'\0'};

u64 gameCardUpdateTitleID = 0;
u32 gameCardUpdateVersion = 0;
char gameCardUpdateVersionStr[128] = {'\0'};

bool isGameCardInserted()
{
    bool inserted;
    if (R_FAILED(fsDeviceOperatorIsGameCardInserted(&fsOperatorInstance, &inserted))) return false;
    return inserted;
}

void fsGameCardDetectionThreadFunc(void *arg)
{
    int idx;
    Result rc;
    
    while(true)
    {
        rc = waitMulti(&idx, -1, waiterForEvent(&fsGameCardKernelEvent), waiterForUEvent(&exitEvent));
        if (R_SUCCEEDED(rc))
        {
            if (idx == 0)
            {
                // Retrieve current gamecard status
                gameCardInserted = isGameCardInserted();
                eventClear(&fsGameCardKernelEvent);
            } else {
                break;
            }
        }
    }
    
    waitMulti(&idx, 0, waiterForEvent(&fsGameCardKernelEvent), waiterForUEvent(&exitEvent));
}

void delay(u8 seconds)
{
    if (!seconds) return;
    
    u64 nanoseconds = seconds * (u64)1000000000;
    svcSleepThread(nanoseconds);
    
    uiRefreshDisplay();
}

bool getGameCardTitleIDAndVersion(u64 *titleID, u32 *version)
{
    bool success = false;
    
    Result result;
    NcmContentMetaDatabase ncmDb;
    
    NcmApplicationContentMetaKey *appList = (NcmApplicationContentMetaKey*)malloc(sizeof(NcmApplicationContentMetaKey));
    if (appList)
    {
        memset(appList, 0, sizeof(NcmApplicationContentMetaKey));
        
        if (R_SUCCEEDED(result = ncmOpenContentMetaDatabase(FsStorageId_GameCard, &ncmDb)))
        {
            if (R_SUCCEEDED(result = ncmContentMetaDatabaseListApplication(&ncmDb, META_DATABASE_FILTER, appList, sizeof(NcmApplicationContentMetaKey), NULL, NULL)))
            {
                *titleID = appList->metaRecord.titleId;
                *version = appList->metaRecord.version;
                success = true;
            } else {
                uiStatusMsg("getGameCardTitleIDAndVersion: ncmContentMetaDatabaseListApplication failed! (0x%08X)", result);
            }
        } else {
            uiStatusMsg("getGameCardTitleIDAndVersion: ncmOpenContentMetaDatabase failed! (0x%08X)", result);
        }
        
        free(appList);
    } else {
        uiStatusMsg("getGameCardTitleIDAndVersion: Unable to allocate memory for the ncm service operations.");
    }
    
    return success;
}

void convertTitleVersionToDecimal(u32 version, char *versionBuf, int versionBufSize)
{
    u8 major = (u8)((version >> 26) & 0x3F);
    u8 middle = (u8)((version >> 20) & 0x3F);
    u8 minor = (u8)((version >> 16) & 0xF);
    u16 build = (u16)version;
    
    snprintf(versionBuf, versionBufSize, "%u (%u.%u.%u.%u)", version, major, middle, minor, build);
}

bool getGameCardControlNacp(u64 titleID, char *nameBuf, int nameBufSize, char *authorBuf, int authorBufSize)
{
    if (titleID == 0) return false;
    
    bool success = false;
    Result result;
    size_t outsize = 0;
    NsApplicationControlData *buf = NULL;
    NacpLanguageEntry *langentry = NULL;
    
    buf = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
    if (buf)
    {
        memset(buf, 0, sizeof(NsApplicationControlData));
        
        if (R_SUCCEEDED(result = nsGetApplicationControlData(1, titleID, buf, sizeof(NsApplicationControlData), &outsize)))
        {
            if (outsize >= sizeof(buf->nacp))
            {
                if (R_SUCCEEDED(result = nacpGetLanguageEntry(&buf->nacp, &langentry)))
                {
                    strncpy(nameBuf, langentry->name, nameBufSize - 1);
                    strncpy(authorBuf, langentry->author, authorBufSize - 1);
                    success = true;
                } else {
                    uiStatusMsg("getGameCardControlNacp: GetLanguageEntry failed! (0x%08X)", result);
                }
            } else {
                uiStatusMsg("getGameCardControlNacp: Control.nacp buffer size (%u bytes) is too small! Expected: %u bytes", outsize, sizeof(buf->nacp));
            }
        } else {
            uiStatusMsg("getGameCardControlNacp: GetApplicationControlData failed! (0x%08X)", result);
        }
        
        free(buf);
    } else {
        uiStatusMsg("getGameCardControlNacp: Unable to allocate memory for the ns service operations.");
    }
    
    return success;
}

void removeIllegalCharacters(char *name)
{
    u32 i, len = strlen(name);
    for (i = 0; i < len; i++)
    {
        if (memchr("?[]/\\=+<>:;\",*|^", name[i], sizeof("?[]/\\=+<>:;\",*|^") - 1) || name[i] < 0x20 || name[i] > 0x7E) name[i] = '_';
    }
}

void strtrim(char *str)
{
    if (!str || !*str) return;
    
    char *start = str;
    char *end = start + strlen(str);
    
    while(--end >= start)
    {
        if (!isspace(*end)) break;
    }
    
    *(++end) = '\0';
    
    while(isspace(*start)) start++;
    
    if (start != str) memmove(str, start, end - start + 1);
}

void loadGameCardInfo()
{
    if (gameCardInserted)
    {
        if (hfs0_header == NULL)
        {
            /* Don't access the gamecard immediately to avoid conflicts with the fsp-srv, ncm and ns services */
            uiPleaseWait();
            
            if (getRootHfs0Header(&fsOperatorInstance))
            {
                if (getGameCardTitleIDAndVersion(&gameCardTitleID, &gameCardVersion))
                {
                    convertTitleVersionToDecimal(gameCardVersion, gameCardVersionStr, sizeof(gameCardVersionStr));
                    
                    getGameCardControlNacp(gameCardTitleID, gameCardName, sizeof(gameCardName), gameCardAuthor, sizeof(gameCardAuthor));
                    
                    strtrim(gameCardName);
                    if (strlen(gameCardName))
                    {
                        snprintf(fixedGameCardName, sizeof(fixedGameCardName) / sizeof(fixedGameCardName[0]), "%s", gameCardName);
                        removeIllegalCharacters(fixedGameCardName);
                    }
                }
            }
            
            uiPrintHeadline();
            uiUpdateStatusMsg();
        }
    } else {
        if (hfs0_header != NULL)
        {
            gameCardSize = 0;
            memset(gameCardSizeStr, 0, sizeof(gameCardSizeStr));
            
            trimmedCardSize = 0;
            memset(trimmedCardSizeStr, 0, sizeof(trimmedCardSizeStr));
            
            free(hfs0_header);
            hfs0_header = NULL;
            hfs0_offset = hfs0_size = 0;
            hfs0_partition_cnt = 0;
            
            if (partitionHfs0Header != NULL)
            {
                free(partitionHfs0Header);
                partitionHfs0Header = NULL;
                partitionHfs0HeaderOffset = 0;
                partitionHfs0HeaderSize = 0;
                partitionHfs0FileCount = 0;
                partitionHfs0StrTableSize = 0;
            }
            
            gameCardTitleID = 0;
            gameCardVersion = 0;
            
            memset(gameCardName, 0, sizeof(gameCardName));
            memset(fixedGameCardName, 0, sizeof(fixedGameCardName));
            memset(gameCardAuthor, 0, sizeof(gameCardAuthor));
            memset(gameCardVersionStr, 0, sizeof(gameCardVersionStr));
            
            gameCardUpdateTitleID = 0;
            gameCardUpdateVersion = 0;
            
            memset(gameCardUpdateVersionStr, 0, sizeof(gameCardUpdateVersionStr));
        }
    }
}

int getSdCardFreeSpace(u64 *out)
{
    struct statvfs st;
    int rc;
    
    rc = statvfs("sdmc:/", &st);
    if (rc != 0)
    {
        uiStatusMsg("getSdCardFreeSpace: Unable to get SD card filesystem stats! statvfs: %d (%s).", errno, strerror(errno));
    } else {
        *out = (u64)(st.f_bsize * st.f_bfree);
    }
    
    return rc;
}

void convertSize(u64 size, char *out, int bufsize)
{
    char buffer[16];
    double bytes = (double)size;
    
    if (bytes < 1000.0)
    {
        snprintf(buffer, sizeof(buffer), "%.0lf B", bytes);
    } else
    if (bytes < 10.0*KiB)
    {
        snprintf(buffer, sizeof(buffer), "%.2lf KiB", floor((bytes*100.0)/KiB)/100.0);
    } else
    if (bytes < 100.0*KiB)
    {
        snprintf(buffer, sizeof(buffer), "%.1lf KiB", floor((bytes*10.0)/KiB)/10.0);
    } else
    if (bytes < 1000.0*KiB)
    {
        snprintf(buffer, sizeof(buffer), "%.0lf KiB", floor(bytes/KiB));
    } else
    if (bytes < 10.0*MiB)
    {
        snprintf(buffer, sizeof(buffer), "%.2lf MiB", floor((bytes*100.0)/MiB)/100.0);
    } else
    if (bytes < 100.0*MiB)
    {
        snprintf(buffer, sizeof(buffer), "%.1lf MiB", floor((bytes*10.0)/MiB)/10.0);
    } else
    if (bytes < 1000.0*MiB)
    {
        snprintf(buffer, sizeof(buffer), "%.0lf MiB", floor(bytes/MiB));
    } else
    if (bytes < 10.0*GiB)
    {
        snprintf(buffer, sizeof(buffer), "%.2lf GiB", floor((bytes*100.0)/GiB)/100.0);
    } else
    if (bytes < 100.0*GiB)
    {
        snprintf(buffer, sizeof(buffer), "%.1lf GiB", floor((bytes*10.0)/GiB)/10.0);
    } else {
        snprintf(buffer, sizeof(buffer), "%.0lf GiB", floor(bytes/GiB));
    }
    
    snprintf(out, bufsize, "%s", buffer);
}

void waitForButtonPress()
{
    uiDrawString("Press any button to continue", 0, breaks * font_height, 255, 255, 255);
    
    uiRefreshDisplay();
    
    while(true)
    {
        hidScanInput();
        u32 keysDown = hidKeysDown(CONTROLLER_P1_AUTO);
        if (keysDown && !((keysDown & KEY_TOUCH) || (keysDown & KEY_LSTICK_LEFT) || (keysDown & KEY_LSTICK_RIGHT) || (keysDown & KEY_LSTICK_UP) || (keysDown & KEY_LSTICK_DOWN) || \
            (keysDown & KEY_RSTICK_LEFT) || (keysDown & KEY_RSTICK_RIGHT) || (keysDown & KEY_RSTICK_UP) || (keysDown & KEY_RSTICK_DOWN))) break;
    }
}

void addStringToFilenameBuffer(const char *string, char **nextFilename)
{
    filenames[filenamesCount++] = *nextFilename;
    strcpy(*nextFilename, string);
    *nextFilename += (strlen(string) + 1);
}

void removeDirectory(const char *path)
{
    struct dirent* ent;
    char cur_path[NAME_BUF_LEN] = {'\0'};
    
    DIR *dir = opendir(path);
    if (dir)
    {
        while ((ent = readdir(dir)) != NULL)
        {
            if ((strlen(ent->d_name) == 1 && !strcmp(ent->d_name, ".")) || (strlen(ent->d_name) == 2 && !strcmp(ent->d_name, ".."))) continue;
            
            snprintf(cur_path, sizeof(cur_path) / sizeof(cur_path[0]), "%s/%s", path, ent->d_name);
            
            if (ent->d_type == DT_DIR)
            {
                removeDirectory(cur_path);
            } else {
                remove(cur_path);
            }
        }
        
        closedir(dir);
        
        rmdir(path);
    }
}

bool parseNSWDBRelease(xmlDocPtr doc, xmlNodePtr cur, u32 crc)
{
    if (!doc || !cur) return false;
    
    xmlChar *key;
    xmlNodePtr node = cur;
    
    u8 imageSize = (u8)(gameCardSize / GAMECARD_SIZE_1GiB);
    
    u8 xmlImageSize = 0;
    u64 xmlTitleID = 0;
    u32 xmlCrc = 0;
    char xmlReleaseName[256] = {'\0'};
    
    bool found = false;
    char strbuf[512] = {'\0'};
    
    while(node != NULL)
    {
        if ((!xmlStrcmp(node->name, (const xmlChar *)nswReleasesChildrenImageSize)))
        {
            key = xmlNodeListGetString(doc, node->xmlChildrenNode, 1);
            if (key)
            {
                xmlImageSize = (u8)atoi((const char*)key);
                xmlFree(key);
            }
        } else
        if ((!xmlStrcmp(node->name, (const xmlChar *)nswReleasesChildrenTitleID)))
        {
            key = xmlNodeListGetString(doc, node->xmlChildrenNode, 1);
            if (key)
            {
                xmlTitleID = strtoull((const char*)key, NULL, 16);
                xmlFree(key);
            }
        } else
        if ((!xmlStrcmp(node->name, (const xmlChar *)nswReleasesChildrenImgCrc)))
        {
            key = xmlNodeListGetString(doc, node->xmlChildrenNode, 1);
            if (key)
            {
                xmlCrc = strtoul((const char*)key, NULL, 16);
                xmlFree(key);
            }
        }
        if ((!xmlStrcmp(node->name, (const xmlChar *)nswReleasesChildrenReleaseName)))
        {
            key = xmlNodeListGetString(doc, node->xmlChildrenNode, 1);
            if (key)
            {
                snprintf(xmlReleaseName, sizeof(xmlReleaseName) / sizeof(xmlReleaseName[0]), "%s", (char*)key);
                xmlFree(key);
            }
        }
        
        node = node->next;
    }
    
    /*if (xmlImageSize && xmlTitleID && strlen(xmlReleaseName))
    {
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Cartridge Image Size: %u.", imageSize);
        uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
        breaks++;
        
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Cartridge Title ID: %016lX.", gameCardTitleID);
        uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
        breaks++;
        
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Cartridge Image CRC32: %08X.", crc);
        uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
        breaks += 2;
        
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "XML Image Size: %u.", xmlImageSize);
        uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
        breaks++;
        
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "XML Title ID: %016lX.", xmlTitleID);
        uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
        breaks++;
        
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "XML Image CRC32: %08X.", xmlCrc);
        uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
        breaks++;
        
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "XML Release Name: %s.", xmlReleaseName);
        uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
        breaks += 2;
    }*/
    
    if (xmlImageSize == imageSize && xmlTitleID == gameCardTitleID && xmlCrc == crc)
    {
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Found matching Scene release: \"%s\" (CRC32: %08X). This is a good dump!", xmlReleaseName, xmlCrc);
        uiDrawString(strbuf, 0, breaks * font_height, 0, 255, 0);
        found = true;
    } else {
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Dump doesn't match Scene release: \"%s\"! (CRC32: %08X)", xmlReleaseName, xmlCrc);
        uiDrawString(strbuf, 0, breaks * font_height, 255, 0, 0);
    }
    
    breaks++;
    
    return found;
}

xmlXPathObjectPtr getNodeSet(xmlDocPtr doc, xmlChar *xpath)
{
    xmlXPathContextPtr context = NULL;
    xmlXPathObjectPtr result = NULL;
    
    context = xmlXPathNewContext(doc);
    result = xmlXPathEvalExpression(xpath, context);
    
    if (xmlXPathNodeSetIsEmpty(result->nodesetval))
    {
        xmlXPathFreeObject(result);
        return NULL;
    }
    
    return result;
}

void gameCardDumpNSWDBCheck(u32 crc)
{
    if (!gameCardTitleID || !hfs0_partition_cnt || !crc) return;
    
    xmlDocPtr doc = NULL;
    bool found = false;
    char strbuf[512] = {'\0'};
    
    doc = xmlParseFile(nswReleasesXmlPath);
    if (doc)
    {
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "//%s/%s[.//%s='%016lX']", nswReleasesRootElement, nswReleasesChildren, nswReleasesChildrenTitleID, gameCardTitleID);
        xmlXPathObjectPtr nodeSet = getNodeSet(doc, (xmlChar*)strbuf);
        if (nodeSet)
        {
            snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Found %d %s with Title ID \"%016lX\".", nodeSet->nodesetval->nodeNr, (nodeSet->nodesetval->nodeNr > 1 ? "releases" : "release"), gameCardTitleID);
            uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
            breaks++;
            
            uiRefreshDisplay();
            
            u32 i;
            for(i = 0; i < nodeSet->nodesetval->nodeNr; i++)
            {
                xmlNodePtr node = nodeSet->nodesetval->nodeTab[i]->xmlChildrenNode;
                
                found = parseNSWDBRelease(doc, node, crc);
                if (found) break;
            }
            
            if (!found)
            {
                uiDrawString("No matches found in XML document! This could either be a bad dump or an undumped cartridge.", 0, breaks * font_height, 255, 0, 0);
            } else {
                breaks--;
            }
            
            xmlXPathFreeObject(nodeSet);
        } else {
            snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Error: unable to find records with Title ID \"%016lX\" within the XML document!", gameCardTitleID);
            uiDrawString(strbuf, 0, breaks * font_height, 255, 0, 0);
        }
        
        xmlFreeDoc(doc);
    } else {
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Error: failed to open and/or parse \"%s\"!", nswReleasesXmlPath);
        uiDrawString(strbuf, 0, breaks * font_height, 255, 0, 0);
    }
}

Result networkInit()
{
    Result result = socketInitializeDefault();
    if (R_SUCCEEDED(result)) curl_global_init(CURL_GLOBAL_ALL);
    return result;
}

void networkDeinit()
{
    curl_global_cleanup();
    socketExit();
}

size_t writeCurlFile(char *buffer, size_t size, size_t number_of_items, void *input_stream)
{
    size_t total_size = (size * number_of_items);
    if (fwrite(buffer, 1, total_size, input_stream) != total_size) return 0;
    return total_size;
}

static size_t writeCurlBuffer(char *buffer, size_t size, size_t number_of_items, void *input_stream)
{
    (void) input_stream;
    const size_t bsz = (size * number_of_items);
    
    if (result_sz == 0 || !result_buf)
    {
        result_sz = 0x1000;
        result_buf = (char*)malloc(result_sz);
        if (!result_buf) return 0;
    }
    
    bool need_realloc = false;
    
    while (result_written + bsz > result_sz) 
    {
        result_sz <<= 1;
        need_realloc = true;
    }
    
    if (need_realloc)
    {
        char *new_buf = (char*)realloc(result_buf, result_sz);
        if (!new_buf) return 0;
        result_buf = new_buf;
    }
    
    memcpy(result_buf + result_written, buffer, bsz);
    result_written += bsz;
    return bsz;
}

void updateNSWDBXml()
{
    Result result;
    CURL *curl;
    CURLcode res;
    long http_code = 0;
    double size = 0.0;
    char strbuf[512] = {'\0'};
    bool success = false;
    
    if (R_SUCCEEDED(result = networkInit()))
    {
        curl = curl_easy_init();
        if (curl)
        {
            FILE *nswdbXml = fopen(nswReleasesXmlTmpPath, "wb");
            if (nswdbXml)
            {
                snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Downloading XML database from \"%s\", please wait...", nswReleasesXmlUrl);
                uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
                breaks += 2;
                
                if (programAppletType != AppletType_Application && programAppletType != AppletType_SystemApplication)
                {
                    uiDrawString("Do not press the HOME button. Doing so could corrupt the SD card filesystem.", 0, breaks * font_height, 255, 0, 0);
                    breaks += 2;
                }
                
                uiRefreshDisplay();
                
                curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 102400L);
                curl_easy_setopt(curl, CURLOPT_URL, nswReleasesXmlUrl);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlFile);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, nswdbXml);
                curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
                curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
                curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
                curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
                
                res = curl_easy_perform(curl);
                
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &size);
                
                if (res == CURLE_OK && http_code >= 200 && http_code <= 299 && size > 0)
                {
                    snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Successfully downloaded %.0lf bytes!", size);
                    uiDrawString(strbuf, 0, breaks * font_height, 0, 255, 0);
                    success = true;
                } else {
                    snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Error: failed to request XML database! HTTP status code: %ld", http_code);
                    uiDrawString(strbuf, 0, breaks * font_height, 255, 0, 0);
                }
                
                fclose(nswdbXml);
                
                if (success)
                {
                    remove(nswReleasesXmlPath);
                    rename(nswReleasesXmlTmpPath, nswReleasesXmlPath);
                } else {
                    remove(nswReleasesXmlTmpPath);
                }
            } else {
                snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Error: failed to open \"%s\" in write mode!", nswReleasesXmlTmpPath);
                uiDrawString(strbuf, 0, breaks * font_height, 255, 0, 0);
            }
            
            curl_easy_cleanup(curl);
        } else {
            uiDrawString("Error: failed to initialize CURL context!", 0, breaks * font_height, 255, 0, 0);
        }
        
        networkDeinit();
    } else {
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Error: failed to initialize socket! (%08X)", result);
        uiDrawString(strbuf, 0, breaks * font_height, 255, 0, 0);
    }
    
    breaks += 2;
}

int versionNumCmp(char *ver1, char *ver2)
{
    int i, curPart, res;
    char *token = NULL;
    
    // Define a struct for comparison purposes
    typedef struct {
        int major;
        int minor;
        int build;
    } version_t;
    
    version_t versionNum1, versionNum2;
    memset(&versionNum1, 0, sizeof(version_t));
    memset(&versionNum2, 0, sizeof(version_t));
    
    // Create copies of the version strings to avoid modifications by strtok()
    char ver1tok[64] = {'\0'};
    snprintf(ver1tok, 63, ver1);
    
    char ver2tok[64] = {'\0'};
    snprintf(ver2tok, 63, ver2);
    
    // Parse version string 1
    i = 0;
    token = strtok(ver1tok, ".");
    while(token != NULL && i < 3)
    {
        curPart = atoi(token);
        
        switch(i)
        {
            case 0:
                versionNum1.major = curPart;
                break;
            case 1:
                versionNum1.minor = curPart;
                break;
            case 2:
                versionNum1.build = curPart;
                break;
            default:
                break;
        }
        
        token = strtok(NULL, ".");
        
        i++;
    }
    
    // Parse version string 2
    i = 0;
    token = strtok(ver2tok, ".");
    while(token != NULL && i < 3)
    {
        curPart = atoi(token);
        
        switch(i)
        {
            case 0:
                versionNum2.major = curPart;
                break;
            case 1:
                versionNum2.minor = curPart;
                break;
            case 2:
                versionNum2.build = curPart;
                break;
            default:
                break;
        }
        
        token = strtok(NULL, ".");
        
        i++;
    }
    
    // Compare version_t structs
    if (versionNum1.major == versionNum2.major)
    {
        if (versionNum1.minor == versionNum2.minor)
        {
            if (versionNum1.build == versionNum2.build)
            {
                res = 0;
            } else
            if (versionNum1.build < versionNum2.build)
            {
                res = -1;
            } else {
                res = 1;
            }
        } else
        if (versionNum1.minor < versionNum2.minor)
        {
            res = -1;
        } else {
            res = 1;
        }
    } else
    if (versionNum1.major < versionNum2.major)
    {
        res = -1;
    } else {
        res = 1;
    }
    
    return res;
}

void updateApplication()
{
    Result result;
    CURL *curl;
    CURLcode res;
    long http_code = 0;
    double size = 0.0;
    char strbuf[1024] = {'\0'}, downloadUrl[512] = {'\0'}, releaseTag[32] = {'\0'};
    bool success = false;
    struct json_object *jobj, *name, *assets;
    FILE *gcDumpToolNro = NULL;
    
    if (R_SUCCEEDED(result = networkInit()))
    {
        curl = curl_easy_init();
        if (curl)
        {
            snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Requesting latest release information from \"%s\"...", githubReleasesApiUrl);
            uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
            breaks++;
            
            uiRefreshDisplay();
            
            curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 102400L);
            curl_easy_setopt(curl, CURLOPT_URL, githubReleasesApiUrl);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlBuffer);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
            curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
            curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
            curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
            
            res = curl_easy_perform(curl);
            
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &size);
            
            if (res == CURLE_OK && http_code >= 200 && http_code <= 299 && size > 0)
            {
                snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Parsing response JSON data from \"%s\"...", githubReleasesApiUrl);
                uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
                breaks++;
                
                uiRefreshDisplay();
                
                jobj = json_tokener_parse(result_buf);
                if (jobj != NULL)
                {
                    if (json_object_object_get_ex(jobj, "name", &name) && json_object_get_type(name) == json_type_string)
                    {
                        snprintf(releaseTag, sizeof(releaseTag) / sizeof(releaseTag[0]), json_object_get_string(name));
                        
                        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Latest release: %s.", releaseTag);
                        uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
                        breaks++;
                        
                        uiRefreshDisplay();
                        
                        // Compare versions
                        if (releaseTag[0] == 'v' || releaseTag[0] == 'V' || releaseTag[0] == 'r' || releaseTag[0] == 'R')
                        {
                            u32 releaseTagLen = strlen(releaseTag);
                            memmove(releaseTag, releaseTag + 1, releaseTagLen - 1);
                            releaseTag[releaseTagLen - 1] = '\0';
                        }
                        
                        if (versionNumCmp(releaseTag, APP_VERSION) > 0)
                        {
                            if (json_object_object_get_ex(jobj, "assets", &assets) && json_object_get_type(assets) == json_type_array)
                            {
                                assets = json_object_array_get_idx(assets, 0);
                                if (assets != NULL)
                                {
                                    if (json_object_object_get_ex(assets, "browser_download_url", &assets) && json_object_get_type(assets) == json_type_string)
                                    {
                                        snprintf(downloadUrl, sizeof(downloadUrl) / sizeof(downloadUrl[0]), json_object_get_string(assets));
                                        
                                        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Download URL: \"%s\".", downloadUrl);
                                        uiDrawString(strbuf, 0, breaks * font_height, 255, 255, 255);
                                        breaks++;
                                        
                                        uiDrawString("Please wait...", 0, breaks * font_height, 255, 255, 255);
                                        breaks += 2;
                                        
                                        if (programAppletType != AppletType_Application && programAppletType != AppletType_SystemApplication)
                                        {
                                            uiDrawString("Do not press the HOME button. Doing so could corrupt the SD card filesystem.", 0, breaks * font_height, 255, 0, 0);
                                            breaks += 2;
                                        }
                                        
                                        uiRefreshDisplay();
                                        
                                        gcDumpToolNro = fopen(gcDumpToolTmpPath, "wb");
                                        if (gcDumpToolNro)
                                        {
                                            curl_easy_reset(curl);
                                            
                                            curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 102400L);
                                            curl_easy_setopt(curl, CURLOPT_URL, downloadUrl);
                                            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlFile);
                                            curl_easy_setopt(curl, CURLOPT_WRITEDATA, gcDumpToolNro);
                                            curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
                                            curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
                                            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
                                            curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
                                            curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
                                            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
                                            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
                                            
                                            res = curl_easy_perform(curl);
                                            
                                            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                                            curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &size);
                                            
                                            if (res == CURLE_OK && http_code >= 200 && http_code <= 299 && size > 0)
                                            {
                                                success = true;
                                                
                                                snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Successfully downloaded %.0lf bytes!", size);
                                                uiDrawString(strbuf, 0, breaks * font_height, 0, 255, 0);
                                                breaks++;
                                                
                                                snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Please restart the application to reflect the changes.", size);
                                                uiDrawString(strbuf, 0, breaks * font_height, 0, 255, 0);
                                            } else {
                                                snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Error: failed to request latest update binary! HTTP status code: %ld", http_code);
                                                uiDrawString(strbuf, 0, breaks * font_height, 255, 0, 0);
                                            }
                                            
                                            fclose(gcDumpToolNro);
                                            
                                            if (success)
                                            {
                                                remove(gcDumpToolPath);
                                                rename(gcDumpToolTmpPath, gcDumpToolPath);
                                            } else {
                                                remove(gcDumpToolTmpPath);
                                            }
                                        } else {
                                            snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Error: failed to open \"%s\" in write mode!", gcDumpToolTmpPath);
                                            uiDrawString(strbuf, 0, breaks * font_height, 255, 0, 0);
                                        }
                                    } else {
                                        uiDrawString("Error: unable to parse download URL from JSON response!", 0, breaks * font_height, 255, 0, 0);
                                    }
                                } else {
                                    uiDrawString("Error: unable to parse object at index 0 from \"assets\" array in JSON response!", 0, breaks * font_height, 255, 0, 0);
                                }
                            } else {
                                uiDrawString("Error: unable to parse \"assets\" array from JSON response!", 0, breaks * font_height, 255, 0, 0);
                            }
                        } else {
                            uiDrawString("You already have the latest version!", 0, breaks * font_height, 255, 255, 255);
                        }
                    } else {
                        uiDrawString("Error: unable to parse version tag from JSON response!", 0, breaks * font_height, 255, 0, 0);
                    }
                    
                    json_object_put(jobj);
                } else {
                    uiDrawString("Error: unable to parse JSON response!", 0, breaks * font_height, 255, 0, 0);
                }
            } else {
                snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Error: failed to request latest release information! HTTP status code: %ld", http_code);
                uiDrawString(strbuf, 0, breaks * font_height, 255, 0, 0);
            }
            
            if (result_buf) free(result_buf);
            
            curl_easy_cleanup(curl);
        } else {
            uiDrawString("Error: failed to initialize CURL context!", 0, breaks * font_height, 255, 0, 0);
        }
        
        networkDeinit();
    } else {
        snprintf(strbuf, sizeof(strbuf) / sizeof(strbuf[0]), "Error: failed to initialize socket! (%08X)", result);
        uiDrawString(strbuf, 0, breaks * font_height, 255, 0, 0);
    }
    
    breaks += 2;
}
