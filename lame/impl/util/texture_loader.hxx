#pragma once
#include <d3d11.h>
#include <wincodec.h>
#include <vector>
#include <string>
#include <wrl/client.h>
#include <objbase.h>

#pragma comment( lib, "Windowscodecs.lib" )

namespace util {
    inline ID3D11ShaderResourceView* load_texture_from_file( ID3D11Device* device, const std::wstring& file_path, int& out_width, int& out_height ) {
        HRESULT hr_co = CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
        bool co_init = ( hr_co == S_OK || hr_co == S_FALSE );

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance( CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( &factory ) );
        if ( FAILED( hr ) ) {
            if ( co_init ) CoUninitialize( );
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromFilename( file_path.c_str( ), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder );
        if ( FAILED( hr ) ) {
            if ( co_init ) CoUninitialize( );
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame( 0, &frame );
        if ( FAILED( hr ) ) {
            if ( co_init ) CoUninitialize( );
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter( &converter );
        if ( FAILED( hr ) ) {
            if ( co_init ) CoUninitialize( );
            return nullptr;
        }

        hr = converter->Initialize( frame.Get( ), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom );
        if ( FAILED( hr ) ) {
            if ( co_init ) CoUninitialize( );
            return nullptr;
        }

        UINT width = 0, height = 0;
        hr = converter->GetSize( &width, &height );
        if ( FAILED( hr ) ) {
            if ( co_init ) CoUninitialize( );
            return nullptr;
        }

        out_width = static_cast<int>( width );
        out_height = static_cast<int>( height );

        std::vector<uint8_t> buffer( width * height * 4 );
        hr = converter->CopyPixels( nullptr, width * 4, static_cast<UINT>( buffer.size( ) ), buffer.data( ) );
        if ( FAILED( hr ) ) {
            if ( co_init ) CoUninitialize( );
            return nullptr;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init_data{};
        init_data.pSysMem = buffer.data( );
        init_data.SysMemPitch = width * 4;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        hr = device->CreateTexture2D( &desc, &init_data, &texture );
        if ( FAILED( hr ) ) {
            if ( co_init ) CoUninitialize( );
            return nullptr;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* srv = nullptr;
        hr = device->CreateShaderResourceView( texture.Get( ), &srv_desc, &srv );
        if ( co_init ) CoUninitialize( );
        if ( FAILED( hr ) ) return nullptr;

        return srv;
    }
}