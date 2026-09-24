/*
  Copyright (C) 2025 The LineageOS Project
  SPDX-License-Identifier: Apache-2.0
*/
CREATE TABLE IF NOT EXISTS qcril_properties_table (property TEXT PRIMARY KEY NOT NULL, def_val TEXT, value TEXT);
INSERT OR REPLACE INTO qcril_properties_table(property, def_val) VALUES('qcrildb_version',12.1);
UPDATE qcril_properties_table SET def_val="0" WHERE property="persist.vendor.radio.poweron_opt";
UPDATE qcril_properties_table SET def_val="false" WHERE property="persist.vendor.radio.do_not_use_ril_optr_db";
UPDATE qcril_properties_table SET def_val="/data/vendor/modem_config/" WHERE property="persist.vendor.radio.mbn_path";
CREATE TABLE IF NOT EXISTS qcril_sw_mbn_mcc_mnc_table (FILE TEXT , MCC TEXT, MNC TEXT, VOLTE_INFO TEXT, MKT_INFO TEXT, LAB_INFO TEXT, PRIMARY KEY(FILE, MCC, MNC));
INSERT OR REPLACE INTO qcril_sw_mbn_mcc_mnc_table (FILE,MCC,MNC,VOLTE_INFO,MKT_INFO,LAB_INFO) VALUES ('mcfg_sw/generic/Korea/KT/Commercial_KT_LTE/mcfg_sw.mbn','450','08','1','1','0');
