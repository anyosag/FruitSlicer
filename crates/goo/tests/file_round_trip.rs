use goo::{
    rle, DelayMode, Error, GooFile, GooWriter, Header, LayerDef, LayerEncoder, Preview, ENDING,
    HEADER_SIZE,
};

fn header(w: u16, h: u16, layers: u32) -> Header {
    Header {
        version: "V3.0".into(),
        software_info: "test".into(),
        software_version: "0".into(),
        file_time: "2026-11-05 00:00:00".into(),
        printer_name: "Test".into(),
        printer_type: "DLP".into(),
        profile_name: "p".into(),
        anti_aliasing_level: 1,
        grey_level: 0,
        blur_level: 0,
        small_preview: Preview::solid(Preview::SMALL, 0x1234),
        big_preview: Preview::solid(Preview::BIG, 0xABCD),
        layer_count: layers,
        resolution_x: w,
        resolution_y: h,
        mirror_x: true,
        mirror_y: false,
        size_x: 10.0,
        size_y: 5.0,
        size_z: 100.0,
        layer_thickness: 0.05,
        exposure_time: 2.5,
        delay_mode: DelayMode::StaticTime,
        turn_off_time: 1.0,
        bottom_before_lift_time: 2.0,
        bottom_after_lift_time: 3.0,
        bottom_after_retract_time: 4.0,
        before_lift_time: 5.0,
        after_lift_time: 6.0,
        after_retract_time: 7.0,
        bottom_exposure_time: 30.0,
        bottom_layers: 1,
        bottom_lift_distance: 8.0,
        bottom_lift_speed: 9.0,
        lift_distance: 10.0,
        lift_speed: 11.0,
        bottom_retract_distance: 12.0,
        bottom_retract_speed: 13.0,
        retract_distance: 14.0,
        retract_speed: 15.0,
        bottom_lift_distance_2: 16.0,
        bottom_lift_speed_2: 17.0,
        lift_distance_2: 18.0,
        lift_speed_2: 19.0,
        bottom_retract_distance_2: 20.0,
        bottom_retract_speed_2: 21.0,
        retract_distance_2: 22.0,
        retract_speed_2: 23.0,
        bottom_light_pwm: 255,
        light_pwm: 200,
        advance_mode: true,
        printing_time: 3600,
        total_volume: 1.5,
        total_weight: 2.5,
        total_price: 3.5,
        price_unit: "$".into(),
        layer_content_offset: HEADER_SIZE,
        grey_scale_level: true,
        transition_layers: 2,
    }
}

fn def(z: f32) -> LayerDef {
    LayerDef {
        pause: false,
        pause_position_z: 100.0,
        position_z: z,
        exposure_time: 2.5,
        off_time: 0.5,
        before_lift_time: 1.0,
        after_lift_time: 2.0,
        after_retract_time: 3.0,
        lift_distance: 4.0,
        lift_speed: 5.0,
        lift_distance_2: 6.0,
        lift_speed_2: 7.0,
        retract_distance: 8.0,
        retract_speed: 9.0,
        retract_distance_2: 10.0,
        retract_speed_2: 11.0,
        light_pwm: 255,
    }
}

#[test]
fn header_is_exactly_spec_size() {
    let mut buf = vec![];
    header(100, 50, 0).write_to(&mut buf).unwrap();
    assert_eq!(buf.len(), HEADER_SIZE as usize);
    // Magic tag sits right after the 4-byte version.
    assert_eq!(&buf[4..12], goo::MAGIC_TAG);
}

#[test]
fn whole_file_round_trips() {
    let (w, h) = (64u16, 32u16);
    let hdr = header(w, h, 3);
    let mut images = vec![];
    let mut writer = GooWriter::new(Vec::new(), &hdr).unwrap();
    for i in 0..3u8 {
        let px: Vec<u8> = (0..w as usize * h as usize)
            .map(|p| {
                if (p / 7 + i as usize) % 3 == 0 {
                    0xFF
                } else {
                    i * 40
                }
            })
            .collect();
        let mut enc = LayerEncoder::new();
        enc.push_pixels(&px);
        let data = enc.finish();
        writer
            .write_layer(&def(0.05 * (i + 1) as f32), &data)
            .unwrap();
        images.push(px);
    }
    let bytes = writer.finish().unwrap();
    assert!(bytes.ends_with(ENDING));

    let file = GooFile::read(bytes.as_slice()).unwrap();
    assert_eq!(file.header, hdr);
    assert_eq!(file.layers.len(), 3);
    for (i, (d, data)) in file.layers.iter().enumerate() {
        assert_eq!(*d, def(0.05 * (i + 1) as f32));
        assert_eq!(
            rle::decode_to_vec(data, w as u64 * h as u64).unwrap(),
            images[i]
        );
    }
}

#[test]
fn writer_rejects_wrong_pixel_count() {
    let hdr = header(10, 10, 1);
    let mut writer = GooWriter::new(Vec::new(), &hdr).unwrap();
    let mut enc = LayerEncoder::new();
    enc.push_run(0, 99);
    assert!(matches!(
        writer.write_layer(&def(0.05), &enc.finish()),
        Err(Error::Invalid(_))
    ));
}

#[test]
fn writer_rejects_missing_layers() {
    let writer = GooWriter::new(Vec::new(), &header(10, 10, 2)).unwrap();
    assert!(writer.finish().is_err());
}

#[test]
fn reader_rejects_truncated_file() {
    let hdr = header(8, 8, 1);
    let mut writer = GooWriter::new(Vec::new(), &hdr).unwrap();
    let mut enc = LayerEncoder::new();
    enc.push_run(0, 64);
    writer.write_layer(&def(0.05), &enc.finish()).unwrap();
    let bytes = writer.finish().unwrap();
    assert!(GooFile::read(&bytes[..bytes.len() - 1]).is_err());
}
