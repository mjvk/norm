#ifndef _NORM_SEGMENT
#define _NORM_SEGMENT

#include "normMessage.h"
#include "normBitmask.h"

// Norm uses preallocated (or dynamically allocated) pools of 
// segments (vectors) for different buffering purposes

class NormSegmentPool
{
    public:
        NormSegmentPool();
        ~NormSegmentPool();
        
        bool Init(unsigned int count, unsigned int size);
        void Destroy();        
        char* Get();
        void Put(char* segment)
        {
            ASSERT(seg_count < seg_total);
            char** ptr = (char**)segment;
            *ptr = seg_list;
            seg_list = segment;
            seg_count++;
        }
        bool IsEmpty() const {return (NULL == seg_list);}
        
        unsigned long CurrentUsage() const {return (seg_total - seg_count);}
        unsigned long PeakUsage() const {return peak_usage;}
        unsigned long OverunCount() const {return overruns;}
        
        unsigned int GetSegmentSize() {return seg_size;}
        
    private: 
        unsigned int    seg_size;
        unsigned int    seg_count;  
        unsigned int    seg_total;
        char*           seg_list;
        
        unsigned long   peak_usage;
        unsigned long   overruns;
        bool            overrun_flag;
};  // end class NormSegmentPool

class NormBlock
{
    friend class NormBlockPool;
    friend class NormBlockBuffer;
    
    public:
        enum Flag 
        {
            IN_REPAIR    = 0x01
        };
            
        NormBlock();
        ~NormBlock();
        const NormBlockId& Id() const {return id;}
        void SetId(NormBlockId& x) {id = x;}
        bool Init(UINT16 blockSize);
        void Destroy();   
        
        void SetFlag(NormBlock::Flag flag) {flags |= flag;}
        bool InRepair() {return (0 != (flags & IN_REPAIR));}
        bool ParityReady(UINT16 ndata) {return (erasure_count == ndata);}
        UINT16 ParityReadiness() {return erasure_count;}
        void IncreaseParityReadiness() {erasure_count++;}
        void SetParityReadiness(UINT16 ndata) {erasure_count = ndata;}
        
        char** SegmentList(UINT16 index = 0) {return &segment_table[index];}
        char* Segment(NormSegmentId sid)
        {
            ASSERT(sid < size);
            return segment_table[sid];
        }
        void AttachSegment(NormSegmentId sid, char* segment)
        {
            ASSERT(sid < size);
            ASSERT(!segment_table[sid]);
            segment_table[sid] = segment;
        }    
        char* DetachSegment(NormSegmentId sid)
        {
            ASSERT(sid < size);
            char* segment = segment_table[sid];
            segment_table[sid] = (char*)NULL;
            return segment;
        }
        void SetSegment(NormSegmentId sid, char* segment)
        {
            ASSERT(sid < size);
            ASSERT(!segment_table[sid]);
            segment_table[sid] = segment;
        }    
        
        // Server routines
        void TxInit(NormBlockId& blockId, UINT16 ndata, UINT16 autoParity)
        {
            id = blockId;
            pending_mask.Clear();
            pending_mask.SetBits(0, ndata+autoParity);
            repair_mask.Clear();
            erasure_count = 0;
            parity_count = 0; 
            parity_offset = autoParity;  
            flags = 0;
        }
        void TxRecover(NormBlockId& blockId, UINT16 ndata, UINT16 nparity)
        {
            id = blockId;
            pending_mask.Clear();
            repair_mask.Clear();
            erasure_count = 0;
            parity_count = nparity;  // force recovered blocks to 
            parity_offset = nparity; // explicit repair mode ???  
            flags = IN_REPAIR;
        }
        bool TxReset(UINT16 ndata, UINT16 nparity, UINT16 autoParity, 
                     UINT16 segmentSize);
        bool TxUpdate(NormSegmentId nextId, NormSegmentId lastId,
                      UINT16 ndata, UINT16 nparity, UINT16 erasureCount);
        
        bool HandleSegmentRequest(NormSegmentId nextId, NormSegmentId lastId,
                                  UINT16 ndata, UINT16 nparity, 
                                  UINT16 erasureCount);
        bool ActivateRepairs(UINT16 nparity);
        void ResetParityCount(UINT16 nparity) 
        {
            parity_offset += parity_count;
            parity_offset = MIN(parity_offset, nparity);
            parity_count = 0;
        }
        bool AppendRepairAdv(NormCmdRepairAdvMsg& cmd, 
                             NormObjectId         objectId,
                             bool                 repairInfo,
                             UINT16               ndata,
                             UINT16               segmentSize);
        
        // Client routines
        void RxInit(NormBlockId& blockId, UINT16 ndata, UINT16 nparity)
        {
            id = blockId;
            pending_mask.Clear();
            pending_mask.SetBits(0, ndata+nparity);
            repair_mask.Clear();
            erasure_count = ndata;
            parity_count = 0;   
            parity_offset = 0;
            flags = 0;
        }
        // Note: This invalidates the repair_mask state.
        bool IsRepairPending(UINT16 ndata, UINT16 nparity); 
        void DecrementErasureCount() {erasure_count--;}
        UINT16 ErasureCount() const {return erasure_count;}
        void IncrementParityCount() {parity_count++;}
        UINT16 ParityCount() const {return parity_count;}
        
        NormSymbolId FirstPending() const
            {return (NormSymbolId)pending_mask.FirstSet();}
        NormSymbolId FirstRepair()  const
            {return (NormSymbolId)repair_mask.FirstSet();}
        bool SetPending(NormSymbolId s) 
            {return pending_mask.Set(s);}
        bool SetPending(NormSymbolId firstId, UINT16 count)
            {return pending_mask.SetBits(firstId, count);}
        void UnsetPending(NormSymbolId s) 
            {pending_mask.Unset(s);}
        void ClearPending()
            {pending_mask.Clear();}
        bool SetRepair(NormSymbolId s) 
            {return repair_mask.Set(s);}
        bool SetRepairs(NormSymbolId first, NormSymbolId last)
        {
            if (first == last)
                return repair_mask.Set(first);
            else
                return (repair_mask.SetBits(first, last-first+1));   
        }
        void UnsetRepair(NormSymbolId s)
            {repair_mask.Unset(s);}
        void ClearRepairs()
            {repair_mask.Clear();}
        bool IsPending(NormSymbolId s) const
            {return pending_mask.Test(s);}
        bool IsPending() const
            {return pending_mask.IsSet();}   
        bool IsRepairPending() const
            {return repair_mask.IsSet();}
        bool IsTransmitPending() const
            {return (pending_mask.IsSet() || repair_mask.IsSet());}
        
            
        NormSymbolId NextPending(NormSymbolId index) const
            {return ((NormSymbolId)pending_mask.NextSet(index));}
        
        
        bool AppendRepairRequest(NormNackMsg&    nack, 
                                 UINT16          ndata, 
                                 UINT16          nparity,
                                 NormObjectId    objectId,
                                 bool            pendingInfo,
                                 UINT16          segmentSize);
        //void DisplayPendingMask(FILE* f) {pending_mask.Display(f);}
        
        bool IsEmpty() const;
        void EmptyToPool(NormSegmentPool& segmentPool);
            
    private:
        NormBlockId id;
        UINT16      size;
        char**      segment_table;
        
        int         flags;
        UINT16      erasure_count;
        UINT16      parity_count;
        UINT16      parity_offset;
        
        NormBitmask pending_mask;
        NormBitmask repair_mask;
        
        NormBlock*  next;
};  // end class NormBlock

class NormBlockPool
{
    public:
        NormBlockPool();
        ~NormBlockPool();
        bool Init(UINT32 numBlocks, UINT16 blockSize);
        void Destroy();
        bool IsEmpty() const {return (NULL == head);}
        NormBlock* Get()
        {
            NormBlock* b = head;
            head = b ? b->next : NULL;
            if (b) 
            {
                overrun_flag = false;
            }
            else if (!overrun_flag)
            {
                overruns++;
                overrun_flag = true;   
            }
            return b;
        }
        void Put(NormBlock* b)
        {
            b->next = head;
            head = b;
        }
        unsigned long OverrunCount() const {return overruns;}
        
    private:
        NormBlock*      head;
        unsigned long   overruns;
        bool            overrun_flag;
};  // end class NormBlockPool

class NormBlockBuffer
{
    public:
        class Iterator;
        friend class NormBlockBuffer::Iterator;
            
        NormBlockBuffer();
        ~NormBlockBuffer();
        bool Init(unsigned long rangeMax, unsigned long tableSize = 256);
        void Destroy();
        
        bool Insert(NormBlock* theBlock);
        bool Remove(const NormBlock* theBlock);
        NormBlock* Find(const NormBlockId& blockId) const;
        
        NormBlockId RangeLo() const {return range_lo;}
        NormBlockId RangeHi() const {return range_hi;}
        bool IsEmpty() const {return (0 == range);}
        bool CanInsert(NormBlockId blockId) const;
        
        class Iterator
        {
            public:
                Iterator(const NormBlockBuffer& blockBuffer);
                NormBlock* GetNextBlock();
                void Reset() {reset = true;}
                
            private:
                const NormBlockBuffer&  buffer;
                bool                    reset;
                NormBlockId             index;
        }; 
            
    private:
        static NormBlock* Next(NormBlock* b) {return b->next;}    
        
        NormBlock**     table;
        unsigned long   hash_mask;       
        unsigned long   range_max;  // max range of blocks that can be buffered
        unsigned long   range;      // zero if "block buffer" is empty
        NormBlockId     range_lo;
        NormBlockId     range_hi;
};  // end class NormBlockBuffer



#endif // _NORM_SEGMENT