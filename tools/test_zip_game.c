/* Production Windows file APIs over an unaligned ZIP entry above 4 GiB. */
#include "../android/host/files.c"
#include <assert.h>
#include <stdatomic.h>

static uint32_t stack, buffer, output, overlap;
static uint32_t image;
uint32_t __wrap_void_module_exe_base(void) { return image; }
static uint64_t offset = (1ull << 32) + 17;
static const uint32_t length = 131077;
static atomic_int copy_probe, audio_progress;
extern int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (atomic_load(&copy_probe)==2) { errno=ENOSPC; return -1; }
    if (atomic_load(&copy_probe)) {
        for (unsigned i=0;i<2000&&!atomic_load(&audio_progress);++i) usleep(1000);
        assert(atomic_load(&audio_progress));
    }
    return __real_fsync(fd);
}
static void *audio_turn(void *arg)
{
    (void)arg;
    while(!atomic_load(&copy_probe)) usleep(1000);
    void_machine_enter();atomic_store(&audio_progress,1);void_machine_leave();
    return NULL;
}
static void args(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    g_esp = stack;
    ARG(0)=a; ARG(1)=b; ARG(2)=c; ARG(3)=d; ARG(4)=e; ARG(5)=f;
}
static void le(FILE *f, uint64_t n, unsigned bytes)
{
    for (unsigned i=0;i<bytes;++i) { assert(fputc((int)(n&255),f)!=EOF); n>>=8; }
}
static void entry(FILE *f, const char *name, uint64_t at, uint64_t size)
{
    le(f,at,8); le(f,size,8); le(f,0,4); le(f,0,4); le(f,0,8); le(f,strlen(name),2);
    assert(fwrite(name,1,strlen(name),f)==strlen(name));
}
static void *reader(void *arg)
{
    const zm_entry *e=arg; uint8_t bytes[257];
    for (unsigned j=0;j<100;++j) {
        uint64_t at=j*997;
        assert(s2_zip_read(e,bytes,sizeof bytes,at)==sizeof bytes);
        for(unsigned i=0;i<sizeof bytes;++i) assert(bytes[i]==(uint8_t)(at+i));
    }
    return NULL;
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0); alarm(60);
    char root[]="/data/local/tmp/sims2/zip-game-XXXXXX"; assert(mkdtemp(root));
    char archive[1200],index[1200],game[1200],profile[1200],path[1600],fdtext[32];
    snprintf(archive,sizeof archive,"%s/archive",root); snprintf(index,sizeof index,"%s/index",root);
    snprintf(game,sizeof game,"%s/game",root); snprintf(profile,sizeof profile,"%s/profile",root);
    int fd=open(archive,O_CREAT|O_EXCL|O_RDWR,0600); assert(fd>=0);
    uint8_t bytes[length]; for(unsigned i=0;i<length;++i) bytes[i]=(uint8_t)i;
    assert(pwrite(fd,bytes,length,(off_t)offset)==length);
    assert(pwrite(fd,"seed!",5,(off_t)(offset+length+19))==5);
    struct stat st; assert(!fstat(fd,&st));
    FILE *z=fopen(index,"wb"); assert(z); assert(fwrite("ZMI1",1,4,z)==4);
    le(z,2,4);le(z,st.st_size,8);for(unsigned i=0;i<32;++i)le(z,0,1);
    entry(z,"Data/File.bin",offset,length);entry(z,"Data/Sub/seed.txt",offset+length+19,5);assert(!fclose(z));
    snprintf(fdtext,sizeof fdtext,"%d",fd);setenv("PORT_MOUNT_GAME",index,1);setenv("PORT_MOUNT_FD_GAME",fdtext,1);setenv("PORT_PART_GAME",game,1);
    assert(void_guest_reserve());
    image=void_vm_alloc(0x400000,0x2000,0x3000u,4u);assert(image==0x400000);
    MEM32(image+0x3C)=0x80;MEM32(image+0x80+0x28)=0x1000;MEM32(image+0x80+0x50)=0x2000;
    void_threads_init(); void_dispatch_init(); void_machine_enter();
    assert(void_files_init(game,profile)); stack=void_galloc(16384)+8192;buffer=void_galloc(length+32);output=void_galloc(1024);overlap=void_galloc(32);
    const char *name="C:\\Games\\The Sims 2 Legacy\\data\\FILE.bin";
    uint32_t a=create_file(name,GENERIC_READ_,OPEN_EXISTING_,0),b=create_file(name,GENERIC_READ_,OPEN_EXISTING_,0);
    assert(a!=WIN_INVALID_HANDLE&&b!=WIN_INVALID_HANDLE&&file_get(a)->zip_entry);
    args(a,buffer,length+8,output,0,0);assert(ReadFile_impl()&&MEM32(output)==length&&!memcmp(GPTR(buffer),bytes,length));
    assert(file_seek(file_get(a),-5,SEEK_END)==length-5);
    args(a,buffer,20,output,0,0);assert(ReadFile_impl()&&MEM32(output)==5&&!memcmp(GPTR(buffer),bytes+length-5,5));
    MEM32(overlap+8)=123;args(b,buffer,17,output,overlap,0);assert(ReadFile_impl()&&MEM32(output)==17&&!memcmp(GPTR(buffer),bytes+123,17)&&file_get(b)->lazy_pos==0);
    assert(file_seek(file_get(a),-1,SEEK_SET)==-1);
    assert(!file_size_of(file_get(a),&st)&&st.st_size==length);
    ino_t identity=st.st_ino;assert(identity);
    snprintf(path,sizeof path,"%s/Data/Sub/seed.txt",game);
    assert(!s2_zip_stat(path,&st)&&st.st_ino&&st.st_ino!=identity);
    snprintf(path,sizeof path,"%s/Data",game);assert(!s2_zip_stat(path,&st)&&st.st_ino&&st.st_ino!=identity);
    uint32_t found=find_first(strdup("C:\\Games\\The Sims 2 Legacy\\Data\\*.*"),output,0);assert(found!=WIN_INVALID_HANDLE);
    unsigned files=0;do { if(!strcmp((char*)GPTR(output+44),"File.bin"))++files;args(found,output,0,0,0,0); }while(FindNextFileA_impl());assert(files==1);void_handle_close(found);
    assert(void_cwd_set("C:\\Games\\The Sims 2 Legacy\\Data\\Sub"));
    pthread_t audio;assert(!pthread_create(&audio,NULL,audio_turn,NULL));atomic_store(&copy_probe,1);
    assert(copy_file("seed.txt","C:\\Users\\Player\\Documents\\copy.txt",1));
    assert(atomic_load(&audio_progress));atomic_store(&copy_probe,0);pthread_join(audio,NULL);
    atomic_store(&copy_probe,2);assert(!copy_file("seed.txt","C:\\Users\\Player\\Documents\\copy.txt",0));atomic_store(&copy_probe,0);
    snprintf(path,sizeof path,"%s/Documents/copy.txt",profile);size_t size;uint8_t *copy=s2_read_file(path,&size);assert(copy&&size==5&&!memcmp(copy,"seed!",5));free(copy);
    args(b,0,2,0,0,0);uint32_t mapping=CreateFileMappingA_impl();assert(mapping);
    args(mapping,4,0,0,0,0);uint32_t view=MapViewOfFile_impl();assert(view&&!memcmp(GPTR(view),bytes,length));assert(void_vm_unmap_file(view));
    args(mapping,4,0,1,16,0);assert(!MapViewOfFile_impl());
    args(mapping,4,0,0,length+1,0);assert(!MapViewOfFile_impl());void_handle_close(mapping);
    pthread_t threads[4];for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],NULL,reader,(void*)file_get(b)->zip_entry));for(unsigned i=0;i<4;++i)pthread_join(threads[i],NULL);
    uint32_t writer=create_file(name,GENERIC_READ_|GENERIC_WRITE_,OPEN_EXISTING_,0);assert(writer!=WIN_INVALID_HANDLE);
    assert(!file_open_now(file_get(writer),writer));assert(!file_get(a)->zip_entry&&!file_get(b)->zip_entry);
    memcpy(GPTR(buffer),"EDIT",4);args(writer,buffer,4,output,0,0);assert(WriteFile_impl()&&MEM32(output)==4);files_flush_all();
    assert(file_seek(file_get(b),0,SEEK_SET)==0);args(b,buffer,4,output,0,0);assert(ReadFile_impl()&&!memcmp(GPTR(buffer),"EDIT",4));
    assert(file_seek(file_get(writer),length+9,SEEK_SET)==length+9);
    args(writer,buffer,1,output,0,0);assert(WriteFile_impl());assert(!file_size_of(file_get(b),&st)&&st.st_size==length+10);
    assert(pread(fd,bytes,4,(off_t)offset)==4&&bytes[0]==0&&bytes[1]==1&&bytes[2]==2&&bytes[3]==3);
    assert(create_file(name,GENERIC_WRITE_,CREATE_NEW_,0)==WIN_INVALID_HANDLE);
    void_handle_close(a);void_handle_close(b);void_handle_close(writer);close(fd);
    puts("PASS production ZIP game files: offsets above 4 GiB, bounded/overlapped reads, independent seeks, union directories, metadata, profile copy releases guest execution for audio, unaligned read-only mappings, concurrent positional reads, lazy copy-on-write and immutable archive");
    return 0;
}
