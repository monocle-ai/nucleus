/**
 * (c) 2015 Alexandro Sanchez Bach. All rights reserved.
 * Released under GPL v2 license. Read LICENSE for more details.
 */

#include "self.h"
#include "nucleus/common.h"
#include "nucleus/config.h"
#include "nucleus/emulator.h"
#include "nucleus/cpu/frontend/ppu/ppu_decoder.h"
#include "nucleus/filesystem/filesystem.h"
#include "nucleus/loader/keys.h"
#include "nucleus/loader/loader.h"

#include "externals/aes.h"
#include "externals/zlib/zlib.h"

#include <cstring>

bool SELFLoader::open(const std::string& path)
{
    FileSystem* fs = getFilesystem(path.c_str());
    File* file = fs->openFile(path, Read);

    if (!fs->isOpen(file)) {
        return false;
    }

    const u64 selfSize = fs->getFileSize(path);
    self.resize(selfSize);
    fs->seekFile(file, 0, SeekSet);
    fs->readFile(file, &self[0], selfSize);
    fs->closeFile(file);

    if (detectFiletype(path) == FILETYPE_SELF) {
        decrypt();
        return true;
    }
    else if (detectFiletype(path) == FILETYPE_ELF) {
        elf.resize(selfSize);
        memcpy(&elf[0], &self[0], selfSize);
        return true;
    }

    return false;
}

bool SELFLoader::load_elf(sys_process_t& proc)
{
    if (elf.empty()) {
        return false;
    }

    const auto& ehdr = (Elf64_Ehdr&)elf[0];

    // Loading program header table
    for (u64 i = 0; i < ehdr.phnum; i++) {
        const auto& phdr = (Elf64_Phdr&)elf[ehdr.phoff + i*sizeof(Elf64_Phdr)];

        switch (phdr.type.ToLE()) {
        case PT_LOAD:
            if (!phdr.memsz) {
                break;
            }

            nucleus.memory(SEG_MAIN_MEMORY).allocFixed(phdr.vaddr, phdr.memsz);
            memcpy(nucleus.memory.ptr(phdr.vaddr), &elf[phdr.offset], phdr.filesz);
            if ((phdr.flags & PF_X) && config.ppuTranslator == PPU_TRANSLATOR_RECOMPILER) {
                auto segment = new cpu::ppu::Segment();
                segment->address = phdr.vaddr;
                segment->size = phdr.filesz;
                segment->analyze();
                segment->recompile();
                nucleus.cell.ppu_segments.push_back(segment);
            }
            break;

        case PT_TLS:
            // TODO: ?
            break;

        case PT_PROC_PARAM:
            if (!phdr.filesz) {
                proc.param.sdk_version = 0xFFFFFFFF;
                proc.param.malloc_pagesize = 0x100000;
            } else {
                proc.param = (sys_process_param_t&)elf[phdr.offset];
            }
            break;

        case PT_PRX_PARAM:
            if (!phdr.filesz) {
                nucleus.log.error(LOG_LOADER, "Invalid PRX_PARAM segment");
            } else {
                proc.prx_param = (sys_process_prx_param_t&)elf[phdr.offset];
            }
            break;
        }
    }
    return true;
}

bool SELFLoader::load_prx(sys_prx_t& prx)
{
    if (elf.empty()) {
        return false;
    }

    const auto& ehdr = (Elf64_Ehdr&)elf[0];
    u32 base_addr;

    // Loading program header table
    for (u64 i = 0; i < ehdr.phnum; i++) {
        const auto& phdr = (Elf64_Phdr&)elf[ehdr.phoff + i*sizeof(Elf64_Phdr)];

        if (phdr.type == PT_LOAD) {
            // Allocate memory and copy segment contents
            const u32 addr = nucleus.memory(SEG_MAIN_MEMORY).alloc(phdr.memsz, 0x10000);
            memcpy(nucleus.memory.ptr(addr), &elf[phdr.offset], phdr.filesz);

            // Add information for PRX Object
            sys_prx_segment_t segment;
            segment.flags = (u32)phdr.flags;
            segment.align = (u32)phdr.align;
            segment.size_file = (u32)phdr.filesz;
            segment.size_memory = (u32)phdr.memsz;
            segment.initial_addr = (u32)phdr.vaddr;
            segment.prx_offset = (u32)phdr.offset;
            segment.addr = addr;
            prx.segments.push_back(segment);

            // Get FNID / addr pairs
            if (phdr.paddr != 0) {
                const auto& module = (sys_prx_module_info_t&)elf[phdr.paddr];
                prx.name = module.name;
                prx.version = module.version;

                // Get FNID / addr pairs
                u32 offset = module.exports_start;
                while (offset < module.exports_end) {
                    const auto& library = (sys_prx_library_info_t&)elf[phdr.offset + offset];
                    offset += library.size;

                    sys_prx_library_t lib;
                    if (library.name_addr) {
                        lib.name = &elf[phdr.offset + library.name_addr];
                    }
                    for (u32 i = 0; i < library.num_func; i++) {
                        const u32 fnid = ((be_t<u32>&)elf[phdr.offset + library.fnid_addr + 4*i]).ToLE();
                        const u32 stub = ((be_t<u32>&)elf[phdr.offset + library.fstub_addr + 4*i]).ToLE();
                        lib.exports[fnid] = stub;
                    }
                    prx.exported_libs.push_back(lib);
                }

                // Patch pointers to other PRXs
                offset = module.imports_start;
                while (offset < module.imports_end) {
                    const auto& library = (sys_prx_library_info_t&)elf[phdr.offset + offset];
                    offset += library.size;

                    sys_prx_library_t lib;
                    if (library.name_addr) {
                        lib.name = &elf[phdr.offset + library.name_addr];
                    }
                    for (u32 i = 0; i < library.num_func; i++) {
                        const u32 fnid = ((be_t<u32>&)elf[phdr.offset + library.fnid_addr + 4*i]).ToLE();
                        const u32 stub = phdr.offset + library.fstub_addr + 4*i;
                        lib.exports[fnid] = stub;
                    }
                    prx.imported_libs.push_back(lib);
                }
            }

            // Update offsets according to the base address where the PRX was allocated
            // TODO: Probably hardcoding i==1 isn't a good idea.
            if (i == 1) {
                // Update FNID / addr pairs
                for (auto& lib : prx.exported_libs) {
                    for (auto& stub : lib.exports) {
                        u32 value = stub.second;
                        for (const auto& segment : prx.segments) {
                            if (segment.initial_addr <= value && value < segment.initial_addr + segment.size_file) {
                                value = value + (segment.addr - segment.initial_addr);
                            }
                        }
                        stub.second = value;
                    }
                }
                // Update FNID / addr pairs
                for (auto& lib : prx.imported_libs) {
                    for (auto& stub : lib.exports) {
                        u32 value = stub.second;
                        for (const auto& segment : prx.segments) {
                            if (segment.prx_offset <= value && value < segment.initial_addr + segment.size_file) {
                                value = value + (segment.addr - segment.initial_addr - 0xf0);
                            }
                        }
                        stub.second = value;
                    }
                }
            }
        }

        if (phdr.type == PT_SCE_PPURELA) {
            for (u32 i = 0; i < phdr.filesz; i += sizeof(sys_prx_relocation_info_t)) {
                const auto& rel = (sys_prx_relocation_info_t&)elf[phdr.offset + i];

                // Address that needs to be patched
                u32 addr = prx.segments[rel.index_addr].addr + rel.offset;
                u32 value = 0;

                switch (rel.type.ToLE()) {
                case R_PPC64_ADDR32:
                    value = (u32)prx.segments[rel.index_value].addr + rel.ptr;
                    nucleus.memory.write32(addr, value);
                    break;

                case R_PPC64_ADDR16_LO:
                    value = (u16)rel.ptr;
                    nucleus.memory.write16(addr, value);
                    break;

                case R_PPC64_ADDR16_HI:
                    value = (u16)(prx.segments[rel.index_value].addr >> 16);
                    nucleus.memory.write16(addr, value);
                    break;

                case R_PPC64_ADDR16_HA:
                    value = (u16)(prx.segments[1].addr >> 16);
                    nucleus.memory.write16(addr, value);
                    break;

                default:
                    nucleus.log.error(LOG_LOADER, "Unknown relocation type!");
                    break;
                }
            }
        }
    }

    for (const auto& importedLib : prx.imported_libs) {

        // TODO: Following libraries are DECR only. Ignore them (TODO: Don't do this)
        if (importedLib.name == "cellLibprof") {
            continue;
        }

        const sys_prx_library_t* targetLibrary = nullptr;

        // Find library (TODO: This is very inefficient)
        for (const auto& object : nucleus.lv2.objects) {
            if (object.second->getType() == SYS_PRX_OBJECT) {
                const auto* imported_prx = (sys_prx_t*)object.second->getData();
                for (const auto& exportedLib : imported_prx->exported_libs) {
                    if (exportedLib.name == importedLib.name) {
                        targetLibrary = &exportedLib;
                        break;
                    }
                }
            }
        }

        if (!targetLibrary) {
            return false;
        }

        for (const auto& import : importedLib.exports) {
            const u32 fnid = import.first;
            nucleus.memory.write32(import.second, targetLibrary->exports.at(fnid));
        }
    }

    // Recompile executable segments
    for (auto& prx_segment : prx.segments) {
        if ((prx_segment.flags & PF_X) && config.ppuTranslator == PPU_TRANSLATOR_RECOMPILER) {
            auto segment = new cpu::ppu::Segment();
            segment->address = prx_segment.addr;
            segment->size = prx_segment.size_file;
            segment->analyze();
            segment->recompile();
            nucleus.cell.ppu_segments.push_back(segment);
        }
    }
    return true;
}

void SELFLoader::close()
{
    elf.clear();
    self.clear();
}

bool SELFLoader::decryptMetadata()
{
    aes_context aes;
    const auto& sce_header = (SceHeader&)self[0x0];
    const auto& self_header = (SelfHeader&)self[0x20];
    const auto& app_info = (AppInfo&)self[0x70];
    const auto key = getSelfKey(app_info.self_type, app_info.version, sce_header.flags);

    auto& meta_info = (MetadataInfo&)self[sizeof(SceHeader) + sce_header.meta];

    // Decrypt NPDRM Layer if necessary
    u8 npdrm_key[0x10];
    u8 npdrm_iv[0x10] = {};
    u32 offset = self_header.controloff;
    while (offset < self_header.controloff + self_header.controlsize) {
        const auto& ctrl = (ControlInfo&)self[offset];
        offset += ctrl.size;
        if (ctrl.type != 3) {
            continue;
        }

        switch (ctrl.npdrm.license.ToLE()) {
        case 1: // Network license
        case 2: // Local license
            nucleus.log.error(LOG_LOADER, "NPDRM Network / Local licenses not yet supported");
            break;
        case 3: // Free license
            memcpy(npdrm_key, NP_KLIC_FREE, 0x10);
            break;
        }

        // Decrypt our key with NP_KLIC_KEY and use it to decrypt the NPDRM Layer of the Metadata Info
        aes_setkey_dec(&aes, NP_KLIC_KEY, 128);
        aes_crypt_ecb(&aes, AES_DECRYPT, npdrm_key, npdrm_key);
        aes_setkey_dec(&aes, npdrm_key, 128);
        aes_crypt_cbc(&aes, AES_DECRYPT, sizeof(MetadataInfo), npdrm_iv, (u8*)&meta_info, (u8*)&meta_info);
        break;
    }

    // Decrypt Metadata Info
    u8 metadata_iv[0x10];
    memcpy(metadata_iv, key.riv, 0x10);
    aes_setkey_dec(&aes, key.erk, 256);
    aes_crypt_cbc(&aes, AES_DECRYPT, sizeof(MetadataInfo), metadata_iv, (u8*)&meta_info, (u8*)&meta_info);

    // Decrypt Metadata Headers (Metadata Header + Metadata Section Headers)
    u8 ctr_stream_block[0x10];
    u8* meta_headers = (u8*)&self[sizeof(SceHeader) + sce_header.meta + sizeof(MetadataInfo)];
    u32 meta_headers_size = sce_header.hsize - (sizeof(SceHeader) + sce_header.meta + sizeof(MetadataInfo));
    size_t ctr_nc_off = 0;
    aes_setkey_enc(&aes, meta_info.key, 128);
    aes_crypt_ctr(&aes, meta_headers_size, &ctr_nc_off, meta_info.iv, ctr_stream_block, meta_headers, meta_headers);

    return true;
}

u32 SELFLoader::getDecryptedElfSize()
{
    u32 size = 0;
    const auto& sce_header = (SceHeader&)self[0x0];
    const auto& self_header = (SelfHeader&)self[0x20];
    const auto& ehdr = (Elf64_Ehdr&)self[self_header.elfoff];

    // Check the maximum offset referenced by the PHDRs in the Metadata Headers
    const u32 meta_header_off = sizeof(SceHeader) + sce_header.meta + sizeof(MetadataInfo);
    const auto& meta_header = (MetadataHeader&)self[meta_header_off];
    for (u32 i = 0; i < meta_header.section_count; i++) {
        const auto& meta_shdr = (MetadataSectionHeader&)self[meta_header_off + sizeof(MetadataHeader) + i*sizeof(MetadataSectionHeader)];
        const auto& meta_phdr = (Elf64_Phdr&)self[self_header.phdroff + meta_shdr.program_idx * sizeof(Elf64_Phdr)];
        if (meta_shdr.type != 2) {
            continue;
        }
        if (meta_shdr.compressed == 2 && size < meta_phdr.offset + meta_phdr.filesz) {
            size = meta_phdr.offset + meta_phdr.filesz;
        }
        else if (size < meta_phdr.offset + meta_shdr.data_size) {
            size = meta_phdr.offset + meta_phdr.filesz;
        }
    }
    // Check the maximum offset referenced by the SHDRs in the EHDR Header
    if (size < ehdr.shoff + ehdr.shnum * sizeof(Elf64_Shdr)) {
        size = ehdr.shoff + ehdr.shnum * sizeof(Elf64_Shdr);
    }
    return size;
}

bool SELFLoader::decrypt()
{
    if (self.empty()) {
        return false;
    }

    const auto& sce_header = (SceHeader&)self[0x0];
    const auto& self_header = (SelfHeader&)self[0x20];

    /**
     * Debug SELF
     */
    if (sce_header.flags == 0x8000) {
        const u64 elfSize = self.size() - sce_header.hsize;
        elf.resize(elfSize);
        memcpy(&elf[0], &self[sce_header.hsize], elfSize);
    }

    /**
     * Retail SELF
     */
    else {
        // Get Metadata Information
        decryptMetadata();
        const u32 meta_header_off = sizeof(SceHeader) + sce_header.meta + sizeof(MetadataInfo);
        const auto& meta_header = (MetadataHeader&)self[meta_header_off];
        const u8* data_keys = (u8*)&self[meta_header_off + sizeof(MetadataHeader) + meta_header.section_count * sizeof(MetadataSectionHeader)];

        // Get ELF size and allocate/initialize it
        const u64 elfSize = getDecryptedElfSize();
        elf.resize(elfSize);

        // Copy the EHDR, PHDRs and SHDRs of the ELF
        const auto& ehdr = (Elf64_Ehdr&)self[self_header.elfoff];
        char* self_phdrs = &self[self_header.phdroff];
        char* self_shdrs = &self[self_header.shdroff];
        char* elf_phdrs = &elf[ehdr.phoff];
        char* elf_shdrs = &elf[ehdr.shoff];
        memcpy(&elf[0], &ehdr, sizeof(Elf64_Ehdr));
        memcpy(elf_phdrs, self_phdrs, ehdr.phnum * sizeof(Elf64_Phdr));
        memcpy(elf_shdrs, self_shdrs, ehdr.shnum * sizeof(Elf64_Shdr));

        // Write Data
        aes_context aes;
        for (u32 i = 0; i < meta_header.section_count; i++) {
            const auto& meta_shdr = (MetadataSectionHeader&)self[meta_header_off + sizeof(MetadataHeader) + i*sizeof(MetadataSectionHeader)];
            const auto& meta_phdr = (Elf64_Phdr&)self[self_header.phdroff + meta_shdr.program_idx * sizeof(Elf64_Phdr)];
            // Check if PHDR type
            if (meta_shdr.type != 2) {
                continue;
            }

            u8* data_decrypted = new u8[meta_shdr.data_size.ToLE()]();
            u8* data_decompressed = new u8[meta_phdr.filesz.ToLE()]();

            // Decrypt if necessary
            if (meta_shdr.encrypted == 3) {
                u8 data_key[0x10];
                u8 data_iv[0x10];
                memcpy(data_key, data_keys + meta_shdr.key_idx * 0x10, 0x10);
                memcpy(data_iv, data_keys + meta_shdr.iv_idx * 0x10, 0x10);
                memcpy(data_decrypted, &self[meta_shdr.data_offset], meta_shdr.data_size);

                // Perform AES-CTR encryption on the data
                u8 ctr_stream_block[0x10] = {};
                size_t ctr_nc_off = 0;
                aes_setkey_enc(&aes, data_key, 128);
                aes_crypt_ctr(&aes, meta_shdr.data_size, &ctr_nc_off, data_iv, ctr_stream_block, data_decrypted, data_decrypted);
            }

            // Decompress if necessary
            if (meta_shdr.compressed == 2) {
                unsigned long length = meta_phdr.filesz;
                uncompress(data_decompressed, &length, data_decrypted, (u32)meta_shdr.data_size);
                memcpy(&elf[meta_phdr.offset], data_decompressed, meta_phdr.filesz);
            }
            else {
                memcpy(&elf[meta_phdr.offset], data_decrypted, meta_shdr.data_size);
            }

            delete[] data_decrypted;
            delete[] data_decompressed;
        }
    }
    return true;
}

u16 SELFLoader::getMachine()
{
    if (elf.empty()) {
        return EM_UNKNOWN;
    }
    const auto& ehdr = (Elf64_Ehdr&)elf[0];
    return ehdr.machine;
}

u64 SELFLoader::getEntry()
{
    if (elf.empty()) {
        return 0;
    }
    const auto& ehdr = (Elf64_Ehdr&)elf[0];
    return ehdr.entry;
}
