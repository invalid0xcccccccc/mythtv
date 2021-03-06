#ifndef PLAYLIST_H_
#define PLAYLIST_H_

// qt
#include <QList>
#include <QMap>

// libmythmetadata
#include <musicmetadata.h>

// mythmusic
#include "playlistcontainer.h"
#include "musicplayer.h"

class PlaylistContainer;
class Playlist;
class MythSystemLegacy;

enum InsertPLOption
{
    PL_REPLACE = 1,
    PL_INSERTATBEGINNING,
    PL_INSERTATEND,
    PL_INSERTAFTERCURRENT
};

enum PlayPLOption
{
    PL_FIRST = 1,
    PL_FIRSTNEW,
    PL_CURRENT
};

struct PlaylistOptions
{
    InsertPLOption insertPLOption;
    PlayPLOption playPLOption;
};

typedef QList<MusicMetadata::IdType> SongList;

class Playlist : public QObject
{
    Q_OBJECT

  public:
    Playlist(void);
    ~Playlist();

    void setParent(PlaylistContainer *myparent) { m_parent = myparent; }

    void loadPlaylist(QString a_name, QString a_host);
    void loadPlaylistByID(int id, QString a_host);

    void savePlaylist(QString a_name, QString a_host);

    void shuffleTracks(MusicPlayer::ShuffleMode mode);

    void describeYourself(void) const; //  debugging

    void fillSongsFromSonglist(QString songList);
    void fillSonglistFromQuery(QString whereClause, 
                               bool removeDuplicates = false,
                               InsertPLOption insertOption = PL_REPLACE,
                               int currentTrackID = 0);
    void fillSonglistFromSmartPlaylist(QString category, QString name,
                                       bool removeDuplicates = false,
                                       InsertPLOption insertOption = PL_REPLACE,
                                       int currentTrackID = 0);
    void fillSonglistFromList(const QList<int> &songList,
                              bool removeDuplicates,
                              InsertPLOption insertOption,
                              int currentTrackID);
    QString toRawSonglist(bool shuffled = false, bool tracksOnly = false);


    MusicMetadata* getSongAt(int pos) const;

    int getTrackCount(void) { return m_songs.count(); }

    void moveTrackUpDown(bool flag, int where_its_at);

    bool checkTrack(MusicMetadata::IdType trackID) const;

    void addTrack(MusicMetadata::IdType trackID, bool update_display);

    int getTrackPosition(MusicMetadata::IdType trackID) { return m_shuffledSongs.indexOf(trackID); }

    void removeTrack(MusicMetadata::IdType trackID);
    void removeAllTracks(void);
    void removeAllCDTracks(void);

    void copyTracks(Playlist *to_ptr, bool update_display);

    bool hasChanged(void) { return m_changed; }
    void changed(void);

    /// whether any changes should be saved to the DB
    void disableSaves(void) { m_doSave = false; }
    void enableSaves(void) { m_doSave = true; }
    bool doSaves(void) { return m_doSave; }

    QString getName(void) { return m_name; } 
    void    setName(QString a_name) { m_name = a_name; }

    bool isActivePlaylist(void) { return m_name == DEFAULT_PLAYLIST_NAME; }

    int  getID(void) { return m_playlistid; }
    void setID(int x) { m_playlistid = x; }

    void getStats(uint *trackCount, uint *totalLength, uint currentTrack = 0, uint *playedLength = nullptr) const;

    void resync(void);

#ifdef CD_WRTITING_FIXED
    void computeSize(double &size_in_MB, double &size_in_sec);
    int CreateCDMP3(void);
    int CreateCDAudio(void);

  private slots:
    void mkisofsData(int fd);
    void cdrecordData(int fd);
    void processExit(uint retval = 0);
#endif

  private:
    MusicMetadata* getRawSongAt(int pos) const;
    QString removeDuplicateTracks(const QString &orig_songlist, const QString &new_songlist);

    int                   m_playlistid;
    QString               m_name;

    SongList              m_songs;
    SongList              m_shuffledSongs;

    PlaylistContainer    *m_parent;
    bool                  m_changed;
    bool                  m_doSave;
#ifdef CD_WRTITING_FIXED
    MythProgressDialog   *m_progress;
    MythSystemLegacy     *m_proc;
    uint                  m_procExitVal;
#endif
};

#endif
