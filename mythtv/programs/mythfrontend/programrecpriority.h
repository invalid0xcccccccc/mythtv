#ifndef PROGRAMRECPROIRITY_H_
#define PROGRAMRECPROIRITY_H_

#include <vector>

#include "recordinginfo.h"
#include "mythscreentype.h"

// mythfrontend
#include "schedulecommon.h"

class QDateTime;

class MythUIButtonList;
class MythUIButtonListItem;
class MythUIText;
class MythUIStateType;
class ProgramRecPriority;

class RecordingRule;

class ProgramRecPriorityInfo : public RecordingInfo
{
    friend class ProgramRecPriority;

  public:
    ProgramRecPriorityInfo();
    ProgramRecPriorityInfo(const ProgramRecPriorityInfo &/*other*/) = default;
    ProgramRecPriorityInfo &operator=(const ProgramRecPriorityInfo &other)
        { clone(other); return *this; }
    ProgramRecPriorityInfo &operator=(const RecordingInfo &other)
        { clone(other); return *this; }
    ProgramRecPriorityInfo &operator=(const ProgramInfo &other)
        { clone(other); return *this; }
    virtual void clone(const ProgramRecPriorityInfo &other,
                       bool ignore_non_serialized_data = false);
    void clone(const RecordingInfo &other,
               bool ignore_non_serialized_data = false) override; // RecordingInfo
    void clone(const ProgramInfo &other,
               bool ignore_non_serialized_data = false) override; // RecordingInfo

    void clear(void) override; // RecordingInfo

    void ToMap(InfoMap &progMap,
               bool showrerecord = false,
               uint star_range = 10) const override; // ProgramInfo

    RecordingType recType;
    int matchCount;
    int recCount;
    QDateTime last_record;
    int avg_delay;
    QString profile;
};

class ProgramRecPriority : public ScheduleCommon
{
    Q_OBJECT
  public:
    ProgramRecPriority(MythScreenStack *parent, const QString &name);
   ~ProgramRecPriority() = default;

    bool Create(void) override; // MythScreenType
    bool keyPressEvent(QKeyEvent *) override; // MythScreenType
    void customEvent(QEvent *event) override; // ScheduleCommon

    enum SortType
    {
        byTitle,
        byRecPriority,
        byRecType,
        byCount,
        byRecCount,
        byLastRecord,
        byAvgDelay
    };

  protected slots:
    void updateInfo(MythUIButtonListItem *item);
    void edit(MythUIButtonListItem *item);
    void scheduleChanged(int recid);

  private:
    void Load(void) override; // MythScreenType
    void Init(void) override; // MythScreenType

    void FillList(void);
    void SortList(ProgramRecPriorityInfo *newCurrentItem = nullptr);
    void UpdateList();
    void RemoveItemFromList(MythUIButtonListItem *item);

    void changeRecPriority(int howMuch);
    void saveRecPriority(void);
    void newTemplate(QString category);
    void remove();
    void deactivate();

    void showMenu(void);
    void showSortMenu(void);

    ProgramInfo *GetCurrentProgram(void) const override; // ScheduleCommon

    QMap<int, ProgramRecPriorityInfo> m_programData;
    std::vector<ProgramRecPriorityInfo*> m_sortedProgram;
    QMap<int, int> m_origRecPriorityData;

    void countMatches(void);
    QMap<int, int> m_conMatch;
    QMap<int, int> m_nowMatch;
    QMap<int, int> m_recMatch;
    QMap<int, int> m_listMatch;

    MythUIButtonList *m_programList;

    MythUIText *m_schedInfoText;
    MythUIText *m_recPriorityText;
    MythUIText *m_recPriorityBText;
    MythUIText *m_finalPriorityText;
    MythUIText *m_lastRecordedText;
    MythUIText *m_lastRecordedDateText;
    MythUIText *m_lastRecordedTimeText;
    MythUIText *m_channameText;
    MythUIText *m_channumText;
    MythUIText *m_callsignText;
    MythUIText *m_recProfileText;

    ProgramRecPriorityInfo *m_currentItem;

    bool m_reverseSort;

    SortType m_sortType;
};

Q_DECLARE_METATYPE(ProgramRecPriorityInfo *)

#endif
