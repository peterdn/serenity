/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <AK/BufferStream.h>
#include <AK/ByteBuffer.h>
#include <AK/FileSystemPath.h>
#include <AK/MappedFile.h>
#include <AK/NonnullOwnPtrVector.h>
#include <LibGfx/GIFLoader.h>
#include <LibGfx/Painter.h>
#include <stdio.h>
#include <string.h>

namespace Gfx {

static RefPtr<Gfx::Bitmap> load_gif_impl(const u8*, size_t);

RefPtr<Gfx::Bitmap> load_gif(const StringView& path)
{
    MappedFile mapped_file(path);
    if (!mapped_file.is_valid())
        return nullptr;
    auto bitmap = load_gif_impl((const u8*)mapped_file.data(), mapped_file.size());
    if (bitmap)
        bitmap->set_mmap_name(String::format("Gfx::Bitmap [%dx%d] - Decoded GIF: %s", bitmap->width(), bitmap->height(), canonicalized_path(path).characters()));
    return bitmap;
}

RefPtr<Gfx::Bitmap> load_gif_from_memory(const u8* data, size_t length)
{
    auto bitmap = load_gif_impl(data, length);
    if (bitmap)
        bitmap->set_mmap_name(String::format("Gfx::Bitmap [%dx%d] - Decoded GIF: <memory>", bitmap->width(), bitmap->height()));
    return bitmap;
}

enum class GIFFormat {
    GIF87a,
    GIF89a,
};

struct RGB {
    u8 r;
    u8 g;
    u8 b;
};

struct LogicalScreen {
    u16 width;
    u16 height;
    RGB color_map[256];
};

struct ImageDescriptor {
    u16 x;
    u16 y;
    u16 width;
    u16 height;
    bool use_global_color_map;
    RGB color_map[256];
    u8 lzw_min_code_size;
    Vector<u8> lzw_encoded_bytes;
};

struct GIFLoadingContext {
    enum State {
        NotDecoded = 0,
        Error,
        HeaderDecoded,
    };
    const u8 *data { nullptr };
    size_t data_size { 0 };
    RefPtr<Gfx::Bitmap> bitmap { nullptr };
};

Optional<GIFFormat> decode_gif_header(BufferStream &stream)
{
    static const char valid_header_87[] = "GIF87a";
    static const char valid_header_89[] = "GIF89a";

    char header[6];
    for (int i = 0; i < 6; ++i)
        stream >> header[i];

    if (!memcmp(header, valid_header_87, sizeof(header)))
        return Optional {GIFFormat::GIF87a};
    else if (!memcmp(header, valid_header_89, sizeof(header)))
        return Optional {GIFFormat::GIF89a};

    return {};
}

int pow2(int n) {
    int p = 1;
    while (n-- > 0) {
        p *= 2;
    }
    return p;
}

RefPtr<Gfx::Bitmap> load_gif_impl(const u8* data, size_t data_size)
{
    if (data_size < 32)
        return nullptr;

    auto buffer = ByteBuffer::wrap(data, data_size);
    BufferStream stream(buffer);
    auto format = decode_gif_header(stream);

    if (!format.has_value()) {
        return nullptr;
    }

    printf("Format is %s\n", format.value() == GIFFormat::GIF89a ? "GIF89a" : "GIF87a");

    LogicalScreen logical_screen;
    stream >> logical_screen.width;
    stream >> logical_screen.height;
    dbg() << "width: " << logical_screen.width << ", height: " << logical_screen.height;
    if (stream.handle_read_failure())
        return nullptr;

    u8 gcm_info = 0;
    stream >> gcm_info;

    if (stream.handle_read_failure())
        return nullptr;

    bool global_color_map_follows_descriptor = gcm_info & 0x80;
    u8 bits_per_pixel = (gcm_info & 7) + 1;
    u8 bits_of_color_resolution = (gcm_info >> 4) & 7;

    printf("LogicalScreen: %dx%d\n", logical_screen.width, logical_screen.height);
    printf("global_color_map_follows_descriptor: %u\n", global_color_map_follows_descriptor);
    printf("bits_per_pixel: %u\n", bits_per_pixel);
    printf("bits_of_color_resolution: %u\n", bits_of_color_resolution);

    u8 background_color = 0;
    stream >> background_color;
    if (stream.handle_read_failure())
        return nullptr;

    printf("background_color: %u\n", background_color);

    u8 pixel_aspect_ratio = 0;
    stream >> pixel_aspect_ratio;
    if (stream.handle_read_failure())
        return nullptr;

    int color_map_entry_count = 1;
    for (int i = 0; i < bits_per_pixel; ++i)
        color_map_entry_count *= 2;

    printf("color_map_entry_count: %d\n", color_map_entry_count);

    dbg() << "color_map_entry_count = " << color_map_entry_count;
    for (int i = 0; i < color_map_entry_count; ++i) {
        stream >> logical_screen.color_map[i].r;
        stream >> logical_screen.color_map[i].g;
        stream >> logical_screen.color_map[i].b;
    }

    if (stream.handle_read_failure())
        return nullptr;

    for (int i = 0; i < color_map_entry_count; ++i) {
        auto& rgb = logical_screen.color_map[i];
        printf("[%02x]: %s\n", i, Color(rgb.r, rgb.g, rgb.b).to_string().characters());
    }

    NonnullOwnPtrVector<ImageDescriptor> images;

    for (;;) {
        u8 sentinel = 0;
        stream >> sentinel;
        printf("Sentinel: %02x\n", sentinel);

        if (sentinel == 0x21) {
            u8 extension_type = 0;
            stream >> extension_type;
            if (stream.handle_read_failure())
                return nullptr;

            printf("Extension block of type %02x\n", extension_type);

            u8 sub_block_length = 0;

            for (;;) {
                stream >> sub_block_length;

                if (stream.handle_read_failure())
                    return nullptr;

                if (sub_block_length == 0)
                    break;

                u8 dummy;
                for (u16 i = 0; i < sub_block_length; ++i)
                    stream >> dummy;

                if (stream.handle_read_failure())
                    return nullptr;
            }
            continue;
        }

        if (sentinel == 0x2c) {
            images.append(make<ImageDescriptor>());
            auto& image = images.last();
            u8 packed_fields { 0 };
            stream >> image.x;
            stream >> image.y;
            stream >> image.width;
            stream >> image.height;
            stream >> packed_fields;
            dbg() << "Packed fields: " << packed_fields;
            if (stream.handle_read_failure())
                return nullptr;
            printf("Image descriptor: %d,%d %dx%d, %02x\n", image.x, image.y, image.width, image.height, packed_fields);

            stream >> image.lzw_min_code_size;

            printf("min code size: %u\n", image.lzw_min_code_size);

            u8 lzw_encoded_bytes_expected = 0;

            for (;;) {
                stream >> lzw_encoded_bytes_expected;

                if (stream.handle_read_failure())
                    return nullptr;

                if (lzw_encoded_bytes_expected == 0)
                    break;

                u8 buffer[256];
                for (int i = 0; i < lzw_encoded_bytes_expected; ++i) {
                    stream >> buffer[i];
                }

                if (stream.handle_read_failure())
                    return nullptr;

                for (int i = 0; i < lzw_encoded_bytes_expected; ++i) {
                    image.lzw_encoded_bytes.append(buffer[i]);
                }
            }
            continue;
        }

        if (sentinel == 0x3b) {
            printf("Trailer! Awesome :)\n");
            break;
        }

        return nullptr;
    }

    // We exited the block loop after finding a trailer. We should have everything needed.
    printf("Image count: %zu\n", images.size());
    if (images.is_empty())
        return nullptr;

    for (size_t i = 0; i < images.size(); ++i) {
        auto& image = images.at(i);
        printf("Image %zu: %d,%d %dx%d  %zu bytes LZW-encoded\n", i, image.x, image.y, image.width, image.height, image.lzw_encoded_bytes.size());

        // FIXME: Decode the LZW-encoded bytes and turn them into an image.
    }

    // return nullptr;

    dbg() << "First image: " << images.first().lzw_encoded_bytes.size() << " bytes, min coding size: " << images.first().lzw_min_code_size;

    // initialise code table
    u16 initial_code_table_size = pow2(images.first().lzw_min_code_size);

    struct CodeTableEntry {
        Vector<u8> colors;
        u16 code;
    };

    Vector<CodeTableEntry> code_table;
    dbg() << "initial_code_table_size: " << initial_code_table_size;
    for (u16 i = 0; i < initial_code_table_size; ++i) {
        code_table.append({{(u8)i}, i});
    }

    // Add clear code
    code_table.append({{0}, initial_code_table_size});
    
    // Add end of image code
    code_table.append({{0}, (u16)((int)initial_code_table_size + 1)});

    int code_size = images.first().lzw_min_code_size + 1;
    Vector<u8> color_stream;
    Vector<u8> prev_output;
    Vector<u8> conjecture;
    // bool started = false;
    int current_bit_index = 0;
    while (true) {
        int shift = (current_bit_index % 8);
        // dbg() << "shift: " << shift;
        u16 mask = (pow2(code_size) - 1) << shift;

        size_t current_byte_index = current_bit_index / 8;

        if (code_size == 11) {
            dbg() << "current_bit_index: " << current_bit_index;
            dbg() << "shift: " << shift;
            dbg() << "mask: " << mask;
            dbg() << "current_byte_index: " << current_byte_index;
        }

        if (current_byte_index >= images.first().lzw_encoded_bytes.size()) {
            dbg() << "PREMATURELY Reached end";
            break;
        }        
        u16* addr = ((u16 *)&images.first().lzw_encoded_bytes.at(current_byte_index));
        u16 tuple = *addr;
        // dbg() << "tuple: " << tuple;

        // dbg() << "current_bit_index: " << current_bit_index;
        // dbg() << "mask: " << mask;
        u16 code = (tuple & mask) >> current_bit_index % 8;

        if (code == initial_code_table_size) {
            dbg() << "CLEARING";
            // TODO
            current_bit_index += code_size;
            code_size = images.first().lzw_min_code_size + 1;

            code_table.clear();
            dbg() << "initial_code_table_size: " << initial_code_table_size;
            for (u16 i = 0; i < initial_code_table_size; ++i) {
                code_table.append({{(u8)i}, i});
            }

            // Add clear code
            code_table.append({{0}, initial_code_table_size});
            
            // Add end of image code
            code_table.append({{0}, (u8)((int)initial_code_table_size + 1)});

            continue;
        } else if (code == initial_code_table_size + 1) {
            dbg() << "END OF INFORMATION";
            break;
        }

        dbg() << "Code: " << code;

        // Lookup code in the table...
        auto entry = code_table.find([&](const auto& v) {
            return v.code == code;
        });

        current_bit_index += code_size;

        Vector<u8> output;
        if (entry != code_table.end()) {
            // dbg() << "Found color string for code " << code;

            output = (*entry).colors;

            // if (!prev_output.is_empty()) {
                conjecture.append(output.at(0));
                
                auto new_entry = code_table.find([&](const auto& v) {
                    return v.colors == conjecture;
                });
                if (new_entry == code_table.end()) {
                    // dbg() << "Adding finalised conjecture to code_table as code: " << (u16)code_table.size();
                    // for (const auto& conj : conjecture) {
                    //     dbg() << conj;
                    // }
                    if (code_table.size() < 4096) {
                        code_table.append({conjecture, (u16)code_table.size()});
                        if ((int)code_table.size() >= pow2(code_size)) {
                            ++code_size;
                            dbg() << "Increased size of code table to: " << code_size << " as code_table.size = " << code_table.size();
                            // if (code_size == 11) {
                            //     break;
                            // }
                            // color_stream.append({0});
                        }
                    } else {
                        dbg() << "Code table max size reached!";
                    }
                }
            // }

            prev_output = output;

        } else {
            if (!prev_output.is_empty()) {
                conjecture.append(prev_output.at(0));
                // dbg() << "Adding finalised conjecture to code_table as code: " << (u16)code_table.size();
                // for (const auto& conj : conjecture) {
                //     dbg() << conj;
                // }
                if (code_table.size() < 4096) {
                    code_table.append({conjecture, (u16)code_table.size()});
                    if ((int)code_table.size() >= pow2(code_size)) {
                        ++code_size;
                        dbg() << "Increased size of code table to: " << code_size;
                        // color_stream.append({0});
                    }
                } else {
                    dbg() << "Code table max size reached!";
                }
            }

            output = conjecture;
            prev_output = output;
        }

        color_stream.append(output);
        dbg() << "color_stream size: " << color_stream.size();
        conjecture = output;

        // started = true;
    }

    auto bitmap = Bitmap::create_purgeable(BitmapFormat::RGBA32, {images.first().width, images.first().height});
    bitmap->fill(Color::from_rgb(0xFF0000));
    
    dbg() << "Total colors: " << color_stream.size();
    for (size_t i = 0; i < color_stream.size(); ++i) {
        auto color = color_stream.at(i);
        auto rgb = logical_screen.color_map[color];
        int x = i % images.first().width;
        int y = i / images.first().width;
        // dbg() << "i: " << i << ", x: " << x << ", y: " << y << ", Color index: " << color << ", actual color: (" << rgb.r << "," << rgb.g << "," << rgb.b << ")";
        
        if (color != 0) {
            Color c = Color(rgb.r, rgb.g, rgb.b);
            bitmap->set_pixel(x, y, c);
        } else {
            Color c = Color(rgb.r, rgb.g, rgb.b);
            bitmap->set_pixel(x, y, c);
        }
    }
    dbg() << "Finished setting bitmap";

    // for (size_t i = 0; i < images.first().lzw_encoded_bytes.size(); ++i) {
    //     auto byte = images.first().lzw_encoded_bytes.at(i);
    //     dbg() << "byte: " << byte;
    // }

    return bitmap;
}

GIFImageDecoderPlugin::GIFImageDecoderPlugin(const u8* data, size_t size)
{
    m_context = make<GIFLoadingContext>();
    m_context->data = data;
    m_context->data_size = size;
}

GIFImageDecoderPlugin::~GIFImageDecoderPlugin() { }

Size GIFImageDecoderPlugin::size() 
{
    if (m_context->bitmap.is_null()) {
        return {};
    }
    
    return { m_context->bitmap->width(), m_context->bitmap->height() };
}

RefPtr<Gfx::Bitmap> GIFImageDecoderPlugin::bitmap() 
{
    if (m_context->bitmap.is_null()) {
        m_context->bitmap = load_gif_impl(m_context->data, m_context->data_size); 
    }
    dbg() << "Returning bitmap with size " << m_context->bitmap->width() << ", " << m_context->bitmap->height();
    return m_context->bitmap;
}

void GIFImageDecoderPlugin::set_volatile() 
{

}

bool GIFImageDecoderPlugin::set_nonvolatile() 
{
    return true;
}

bool GIFImageDecoderPlugin::sniff() {
    auto buffer = ByteBuffer::wrap(m_context->data, m_context->data_size);
    BufferStream stream(buffer);
    return decode_gif_header(stream).has_value();
}

}
