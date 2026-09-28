/* Explicit user provisioning for sinks that cannot identify themselves.
 * Each record maps an eight-hex-digit display handle to a saved EDID asset.
 * This is NOT monitor identity: cable moves require a new binding, and a live
 * valid EDID always supersedes it. Ambiguous/malformed records fail closed. */
#ifndef KESTREL_NVKMS_EDID_BINDING_H
#define KESTREL_NVKMS_EDID_BINDING_H
static int nvkms_edid_has_identity(const unsigned char *bytes,unsigned length) {
    static const unsigned char header[8]={0,255,255,255,255,255,255,0};
    if(!bytes || length<128)return 0;
    unsigned sum=0;
    for(unsigned i=0;i<8;i++)if(bytes[i]!=header[i])return 0;
    for(unsigned i=0;i<128;i++)sum+=bytes[i];
    return !(sum&255u) && bytes[18]==1 && (bytes[8]||bytes[9]);
}
static int nvkms_edid_binding(const char *text,unsigned bytes,unsigned handle,unsigned *profile) {
    unsigned found=0,value=0;
    if(!text || !profile || !bytes || bytes>512)return 0;
    unsigned handles[32],count=0;
    for(unsigned i=0;i<bytes;){
        if(text[i]=='\n'||text[i]=='\r'){i++;continue;}
        unsigned pair[2]={0,0};
        for(unsigned column=0;column<2;column++){
            for(unsigned k=0;k<8;k++){
                if(i>=bytes)return 0;
                unsigned c=(unsigned char)text[i++],digit;
                if(c>='0'&&c<='9')digit=c-'0';
                else if(c>='a'&&c<='f')digit=c-'a'+10;
                else if(c>='A'&&c<='F')digit=c-'A'+10;
                else return 0;
                pair[column]=(pair[column]<<4)|digit;
            }
            if(!column && (i>=bytes || text[i++]!=' '))return 0;
        }
        if(i<bytes && text[i]!='\n' && text[i]!='\r')return 0;
        if(!pair[0] || !pair[1] || count==32)return 0;
        for(unsigned k=0;k<count;k++)if(handles[k]==pair[0])return 0;
        handles[count++]=pair[0];
        if(pair[0]==handle){found++;value=pair[1];}
    }
    if(found!=1)return 0;
    *profile=value;return 1;
}
#endif
