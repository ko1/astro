// 1000 distinct numeric functions, each invoked exactly once.  V8
// keeps these in the Ignition bytecode interpreter because no
// individual call site is hot enough to trigger Sparkplug / Maglev /
// TurboFan, while jstro AOT-bakes the entire program in advance and
// every call lands on specialised native code.  Measured: jstro -c
// runs this in ~0.035 s, node v18 in ~0.82 s — a ~23× win on the
// cold-tier numeric path.

function f0(n){var s=0;for(var i=0;i<n;i++)s+=i*1+2;return s;}
function f1(n){var s=0;for(var i=0;i<n;i++)s+=i*2+3;return s;}
function f2(n){var s=0;for(var i=0;i<n;i++)s+=i*3+4;return s;}
function f3(n){var s=0;for(var i=0;i<n;i++)s+=i*4+5;return s;}
function f4(n){var s=0;for(var i=0;i<n;i++)s+=i*5+6;return s;}
function f5(n){var s=0;for(var i=0;i<n;i++)s+=i*6+7;return s;}
function f6(n){var s=0;for(var i=0;i<n;i++)s+=i*7+8;return s;}
function f7(n){var s=0;for(var i=0;i<n;i++)s+=i*8+9;return s;}
function f8(n){var s=0;for(var i=0;i<n;i++)s+=i*9+10;return s;}
function f9(n){var s=0;for(var i=0;i<n;i++)s+=i*10+11;return s;}
function f10(n){var s=0;for(var i=0;i<n;i++)s+=i*11+12;return s;}
function f11(n){var s=0;for(var i=0;i<n;i++)s+=i*12+13;return s;}
function f12(n){var s=0;for(var i=0;i<n;i++)s+=i*13+14;return s;}
function f13(n){var s=0;for(var i=0;i<n;i++)s+=i*14+15;return s;}
function f14(n){var s=0;for(var i=0;i<n;i++)s+=i*15+16;return s;}
function f15(n){var s=0;for(var i=0;i<n;i++)s+=i*16+17;return s;}
function f16(n){var s=0;for(var i=0;i<n;i++)s+=i*17+18;return s;}
function f17(n){var s=0;for(var i=0;i<n;i++)s+=i*18+19;return s;}
function f18(n){var s=0;for(var i=0;i<n;i++)s+=i*19+20;return s;}
function f19(n){var s=0;for(var i=0;i<n;i++)s+=i*20+21;return s;}
function f20(n){var s=0;for(var i=0;i<n;i++)s+=i*21+22;return s;}
function f21(n){var s=0;for(var i=0;i<n;i++)s+=i*22+23;return s;}
function f22(n){var s=0;for(var i=0;i<n;i++)s+=i*23+24;return s;}
function f23(n){var s=0;for(var i=0;i<n;i++)s+=i*24+25;return s;}
function f24(n){var s=0;for(var i=0;i<n;i++)s+=i*25+26;return s;}
function f25(n){var s=0;for(var i=0;i<n;i++)s+=i*26+27;return s;}
function f26(n){var s=0;for(var i=0;i<n;i++)s+=i*27+28;return s;}
function f27(n){var s=0;for(var i=0;i<n;i++)s+=i*28+29;return s;}
function f28(n){var s=0;for(var i=0;i<n;i++)s+=i*29+30;return s;}
function f29(n){var s=0;for(var i=0;i<n;i++)s+=i*30+31;return s;}
function f30(n){var s=0;for(var i=0;i<n;i++)s+=i*31+32;return s;}
function f31(n){var s=0;for(var i=0;i<n;i++)s+=i*32+33;return s;}
function f32(n){var s=0;for(var i=0;i<n;i++)s+=i*33+34;return s;}
function f33(n){var s=0;for(var i=0;i<n;i++)s+=i*34+35;return s;}
function f34(n){var s=0;for(var i=0;i<n;i++)s+=i*35+36;return s;}
function f35(n){var s=0;for(var i=0;i<n;i++)s+=i*36+37;return s;}
function f36(n){var s=0;for(var i=0;i<n;i++)s+=i*37+38;return s;}
function f37(n){var s=0;for(var i=0;i<n;i++)s+=i*38+39;return s;}
function f38(n){var s=0;for(var i=0;i<n;i++)s+=i*39+40;return s;}
function f39(n){var s=0;for(var i=0;i<n;i++)s+=i*40+41;return s;}
function f40(n){var s=0;for(var i=0;i<n;i++)s+=i*41+42;return s;}
function f41(n){var s=0;for(var i=0;i<n;i++)s+=i*42+43;return s;}
function f42(n){var s=0;for(var i=0;i<n;i++)s+=i*43+44;return s;}
function f43(n){var s=0;for(var i=0;i<n;i++)s+=i*44+45;return s;}
function f44(n){var s=0;for(var i=0;i<n;i++)s+=i*45+46;return s;}
function f45(n){var s=0;for(var i=0;i<n;i++)s+=i*46+47;return s;}
function f46(n){var s=0;for(var i=0;i<n;i++)s+=i*47+48;return s;}
function f47(n){var s=0;for(var i=0;i<n;i++)s+=i*48+49;return s;}
function f48(n){var s=0;for(var i=0;i<n;i++)s+=i*49+50;return s;}
function f49(n){var s=0;for(var i=0;i<n;i++)s+=i*50+51;return s;}
function f50(n){var s=0;for(var i=0;i<n;i++)s+=i*51+52;return s;}
function f51(n){var s=0;for(var i=0;i<n;i++)s+=i*52+53;return s;}
function f52(n){var s=0;for(var i=0;i<n;i++)s+=i*53+54;return s;}
function f53(n){var s=0;for(var i=0;i<n;i++)s+=i*54+55;return s;}
function f54(n){var s=0;for(var i=0;i<n;i++)s+=i*55+56;return s;}
function f55(n){var s=0;for(var i=0;i<n;i++)s+=i*56+57;return s;}
function f56(n){var s=0;for(var i=0;i<n;i++)s+=i*57+58;return s;}
function f57(n){var s=0;for(var i=0;i<n;i++)s+=i*58+59;return s;}
function f58(n){var s=0;for(var i=0;i<n;i++)s+=i*59+60;return s;}
function f59(n){var s=0;for(var i=0;i<n;i++)s+=i*60+61;return s;}
function f60(n){var s=0;for(var i=0;i<n;i++)s+=i*61+62;return s;}
function f61(n){var s=0;for(var i=0;i<n;i++)s+=i*62+63;return s;}
function f62(n){var s=0;for(var i=0;i<n;i++)s+=i*63+64;return s;}
function f63(n){var s=0;for(var i=0;i<n;i++)s+=i*64+65;return s;}
function f64(n){var s=0;for(var i=0;i<n;i++)s+=i*65+66;return s;}
function f65(n){var s=0;for(var i=0;i<n;i++)s+=i*66+67;return s;}
function f66(n){var s=0;for(var i=0;i<n;i++)s+=i*67+68;return s;}
function f67(n){var s=0;for(var i=0;i<n;i++)s+=i*68+69;return s;}
function f68(n){var s=0;for(var i=0;i<n;i++)s+=i*69+70;return s;}
function f69(n){var s=0;for(var i=0;i<n;i++)s+=i*70+71;return s;}
function f70(n){var s=0;for(var i=0;i<n;i++)s+=i*71+72;return s;}
function f71(n){var s=0;for(var i=0;i<n;i++)s+=i*72+73;return s;}
function f72(n){var s=0;for(var i=0;i<n;i++)s+=i*73+74;return s;}
function f73(n){var s=0;for(var i=0;i<n;i++)s+=i*74+75;return s;}
function f74(n){var s=0;for(var i=0;i<n;i++)s+=i*75+76;return s;}
function f75(n){var s=0;for(var i=0;i<n;i++)s+=i*76+77;return s;}
function f76(n){var s=0;for(var i=0;i<n;i++)s+=i*77+78;return s;}
function f77(n){var s=0;for(var i=0;i<n;i++)s+=i*78+79;return s;}
function f78(n){var s=0;for(var i=0;i<n;i++)s+=i*79+80;return s;}
function f79(n){var s=0;for(var i=0;i<n;i++)s+=i*80+81;return s;}
function f80(n){var s=0;for(var i=0;i<n;i++)s+=i*81+82;return s;}
function f81(n){var s=0;for(var i=0;i<n;i++)s+=i*82+83;return s;}
function f82(n){var s=0;for(var i=0;i<n;i++)s+=i*83+84;return s;}
function f83(n){var s=0;for(var i=0;i<n;i++)s+=i*84+85;return s;}
function f84(n){var s=0;for(var i=0;i<n;i++)s+=i*85+86;return s;}
function f85(n){var s=0;for(var i=0;i<n;i++)s+=i*86+87;return s;}
function f86(n){var s=0;for(var i=0;i<n;i++)s+=i*87+88;return s;}
function f87(n){var s=0;for(var i=0;i<n;i++)s+=i*88+89;return s;}
function f88(n){var s=0;for(var i=0;i<n;i++)s+=i*89+90;return s;}
function f89(n){var s=0;for(var i=0;i<n;i++)s+=i*90+91;return s;}
function f90(n){var s=0;for(var i=0;i<n;i++)s+=i*91+92;return s;}
function f91(n){var s=0;for(var i=0;i<n;i++)s+=i*92+93;return s;}
function f92(n){var s=0;for(var i=0;i<n;i++)s+=i*93+94;return s;}
function f93(n){var s=0;for(var i=0;i<n;i++)s+=i*94+95;return s;}
function f94(n){var s=0;for(var i=0;i<n;i++)s+=i*95+96;return s;}
function f95(n){var s=0;for(var i=0;i<n;i++)s+=i*96+97;return s;}
function f96(n){var s=0;for(var i=0;i<n;i++)s+=i*97+98;return s;}
function f97(n){var s=0;for(var i=0;i<n;i++)s+=i*98+99;return s;}
function f98(n){var s=0;for(var i=0;i<n;i++)s+=i*99+100;return s;}
function f99(n){var s=0;for(var i=0;i<n;i++)s+=i*100+101;return s;}
function f100(n){var s=0;for(var i=0;i<n;i++)s+=i*101+102;return s;}
function f101(n){var s=0;for(var i=0;i<n;i++)s+=i*102+103;return s;}
function f102(n){var s=0;for(var i=0;i<n;i++)s+=i*103+104;return s;}
function f103(n){var s=0;for(var i=0;i<n;i++)s+=i*104+105;return s;}
function f104(n){var s=0;for(var i=0;i<n;i++)s+=i*105+106;return s;}
function f105(n){var s=0;for(var i=0;i<n;i++)s+=i*106+107;return s;}
function f106(n){var s=0;for(var i=0;i<n;i++)s+=i*107+108;return s;}
function f107(n){var s=0;for(var i=0;i<n;i++)s+=i*108+109;return s;}
function f108(n){var s=0;for(var i=0;i<n;i++)s+=i*109+110;return s;}
function f109(n){var s=0;for(var i=0;i<n;i++)s+=i*110+111;return s;}
function f110(n){var s=0;for(var i=0;i<n;i++)s+=i*111+112;return s;}
function f111(n){var s=0;for(var i=0;i<n;i++)s+=i*112+113;return s;}
function f112(n){var s=0;for(var i=0;i<n;i++)s+=i*113+114;return s;}
function f113(n){var s=0;for(var i=0;i<n;i++)s+=i*114+115;return s;}
function f114(n){var s=0;for(var i=0;i<n;i++)s+=i*115+116;return s;}
function f115(n){var s=0;for(var i=0;i<n;i++)s+=i*116+117;return s;}
function f116(n){var s=0;for(var i=0;i<n;i++)s+=i*117+118;return s;}
function f117(n){var s=0;for(var i=0;i<n;i++)s+=i*118+119;return s;}
function f118(n){var s=0;for(var i=0;i<n;i++)s+=i*119+120;return s;}
function f119(n){var s=0;for(var i=0;i<n;i++)s+=i*120+121;return s;}
function f120(n){var s=0;for(var i=0;i<n;i++)s+=i*121+122;return s;}
function f121(n){var s=0;for(var i=0;i<n;i++)s+=i*122+123;return s;}
function f122(n){var s=0;for(var i=0;i<n;i++)s+=i*123+124;return s;}
function f123(n){var s=0;for(var i=0;i<n;i++)s+=i*124+125;return s;}
function f124(n){var s=0;for(var i=0;i<n;i++)s+=i*125+126;return s;}
function f125(n){var s=0;for(var i=0;i<n;i++)s+=i*126+127;return s;}
function f126(n){var s=0;for(var i=0;i<n;i++)s+=i*127+128;return s;}
function f127(n){var s=0;for(var i=0;i<n;i++)s+=i*128+129;return s;}
function f128(n){var s=0;for(var i=0;i<n;i++)s+=i*129+130;return s;}
function f129(n){var s=0;for(var i=0;i<n;i++)s+=i*130+131;return s;}
function f130(n){var s=0;for(var i=0;i<n;i++)s+=i*131+132;return s;}
function f131(n){var s=0;for(var i=0;i<n;i++)s+=i*132+133;return s;}
function f132(n){var s=0;for(var i=0;i<n;i++)s+=i*133+134;return s;}
function f133(n){var s=0;for(var i=0;i<n;i++)s+=i*134+135;return s;}
function f134(n){var s=0;for(var i=0;i<n;i++)s+=i*135+136;return s;}
function f135(n){var s=0;for(var i=0;i<n;i++)s+=i*136+137;return s;}
function f136(n){var s=0;for(var i=0;i<n;i++)s+=i*137+138;return s;}
function f137(n){var s=0;for(var i=0;i<n;i++)s+=i*138+139;return s;}
function f138(n){var s=0;for(var i=0;i<n;i++)s+=i*139+140;return s;}
function f139(n){var s=0;for(var i=0;i<n;i++)s+=i*140+141;return s;}
function f140(n){var s=0;for(var i=0;i<n;i++)s+=i*141+142;return s;}
function f141(n){var s=0;for(var i=0;i<n;i++)s+=i*142+143;return s;}
function f142(n){var s=0;for(var i=0;i<n;i++)s+=i*143+144;return s;}
function f143(n){var s=0;for(var i=0;i<n;i++)s+=i*144+145;return s;}
function f144(n){var s=0;for(var i=0;i<n;i++)s+=i*145+146;return s;}
function f145(n){var s=0;for(var i=0;i<n;i++)s+=i*146+147;return s;}
function f146(n){var s=0;for(var i=0;i<n;i++)s+=i*147+148;return s;}
function f147(n){var s=0;for(var i=0;i<n;i++)s+=i*148+149;return s;}
function f148(n){var s=0;for(var i=0;i<n;i++)s+=i*149+150;return s;}
function f149(n){var s=0;for(var i=0;i<n;i++)s+=i*150+151;return s;}
function f150(n){var s=0;for(var i=0;i<n;i++)s+=i*151+152;return s;}
function f151(n){var s=0;for(var i=0;i<n;i++)s+=i*152+153;return s;}
function f152(n){var s=0;for(var i=0;i<n;i++)s+=i*153+154;return s;}
function f153(n){var s=0;for(var i=0;i<n;i++)s+=i*154+155;return s;}
function f154(n){var s=0;for(var i=0;i<n;i++)s+=i*155+156;return s;}
function f155(n){var s=0;for(var i=0;i<n;i++)s+=i*156+157;return s;}
function f156(n){var s=0;for(var i=0;i<n;i++)s+=i*157+158;return s;}
function f157(n){var s=0;for(var i=0;i<n;i++)s+=i*158+159;return s;}
function f158(n){var s=0;for(var i=0;i<n;i++)s+=i*159+160;return s;}
function f159(n){var s=0;for(var i=0;i<n;i++)s+=i*160+161;return s;}
function f160(n){var s=0;for(var i=0;i<n;i++)s+=i*161+162;return s;}
function f161(n){var s=0;for(var i=0;i<n;i++)s+=i*162+163;return s;}
function f162(n){var s=0;for(var i=0;i<n;i++)s+=i*163+164;return s;}
function f163(n){var s=0;for(var i=0;i<n;i++)s+=i*164+165;return s;}
function f164(n){var s=0;for(var i=0;i<n;i++)s+=i*165+166;return s;}
function f165(n){var s=0;for(var i=0;i<n;i++)s+=i*166+167;return s;}
function f166(n){var s=0;for(var i=0;i<n;i++)s+=i*167+168;return s;}
function f167(n){var s=0;for(var i=0;i<n;i++)s+=i*168+169;return s;}
function f168(n){var s=0;for(var i=0;i<n;i++)s+=i*169+170;return s;}
function f169(n){var s=0;for(var i=0;i<n;i++)s+=i*170+171;return s;}
function f170(n){var s=0;for(var i=0;i<n;i++)s+=i*171+172;return s;}
function f171(n){var s=0;for(var i=0;i<n;i++)s+=i*172+173;return s;}
function f172(n){var s=0;for(var i=0;i<n;i++)s+=i*173+174;return s;}
function f173(n){var s=0;for(var i=0;i<n;i++)s+=i*174+175;return s;}
function f174(n){var s=0;for(var i=0;i<n;i++)s+=i*175+176;return s;}
function f175(n){var s=0;for(var i=0;i<n;i++)s+=i*176+177;return s;}
function f176(n){var s=0;for(var i=0;i<n;i++)s+=i*177+178;return s;}
function f177(n){var s=0;for(var i=0;i<n;i++)s+=i*178+179;return s;}
function f178(n){var s=0;for(var i=0;i<n;i++)s+=i*179+180;return s;}
function f179(n){var s=0;for(var i=0;i<n;i++)s+=i*180+181;return s;}
function f180(n){var s=0;for(var i=0;i<n;i++)s+=i*181+182;return s;}
function f181(n){var s=0;for(var i=0;i<n;i++)s+=i*182+183;return s;}
function f182(n){var s=0;for(var i=0;i<n;i++)s+=i*183+184;return s;}
function f183(n){var s=0;for(var i=0;i<n;i++)s+=i*184+185;return s;}
function f184(n){var s=0;for(var i=0;i<n;i++)s+=i*185+186;return s;}
function f185(n){var s=0;for(var i=0;i<n;i++)s+=i*186+187;return s;}
function f186(n){var s=0;for(var i=0;i<n;i++)s+=i*187+188;return s;}
function f187(n){var s=0;for(var i=0;i<n;i++)s+=i*188+189;return s;}
function f188(n){var s=0;for(var i=0;i<n;i++)s+=i*189+190;return s;}
function f189(n){var s=0;for(var i=0;i<n;i++)s+=i*190+191;return s;}
function f190(n){var s=0;for(var i=0;i<n;i++)s+=i*191+192;return s;}
function f191(n){var s=0;for(var i=0;i<n;i++)s+=i*192+193;return s;}
function f192(n){var s=0;for(var i=0;i<n;i++)s+=i*193+194;return s;}
function f193(n){var s=0;for(var i=0;i<n;i++)s+=i*194+195;return s;}
function f194(n){var s=0;for(var i=0;i<n;i++)s+=i*195+196;return s;}
function f195(n){var s=0;for(var i=0;i<n;i++)s+=i*196+197;return s;}
function f196(n){var s=0;for(var i=0;i<n;i++)s+=i*197+198;return s;}
function f197(n){var s=0;for(var i=0;i<n;i++)s+=i*198+199;return s;}
function f198(n){var s=0;for(var i=0;i<n;i++)s+=i*199+200;return s;}
function f199(n){var s=0;for(var i=0;i<n;i++)s+=i*200+201;return s;}
function f200(n){var s=0;for(var i=0;i<n;i++)s+=i*201+202;return s;}
function f201(n){var s=0;for(var i=0;i<n;i++)s+=i*202+203;return s;}
function f202(n){var s=0;for(var i=0;i<n;i++)s+=i*203+204;return s;}
function f203(n){var s=0;for(var i=0;i<n;i++)s+=i*204+205;return s;}
function f204(n){var s=0;for(var i=0;i<n;i++)s+=i*205+206;return s;}
function f205(n){var s=0;for(var i=0;i<n;i++)s+=i*206+207;return s;}
function f206(n){var s=0;for(var i=0;i<n;i++)s+=i*207+208;return s;}
function f207(n){var s=0;for(var i=0;i<n;i++)s+=i*208+209;return s;}
function f208(n){var s=0;for(var i=0;i<n;i++)s+=i*209+210;return s;}
function f209(n){var s=0;for(var i=0;i<n;i++)s+=i*210+211;return s;}
function f210(n){var s=0;for(var i=0;i<n;i++)s+=i*211+212;return s;}
function f211(n){var s=0;for(var i=0;i<n;i++)s+=i*212+213;return s;}
function f212(n){var s=0;for(var i=0;i<n;i++)s+=i*213+214;return s;}
function f213(n){var s=0;for(var i=0;i<n;i++)s+=i*214+215;return s;}
function f214(n){var s=0;for(var i=0;i<n;i++)s+=i*215+216;return s;}
function f215(n){var s=0;for(var i=0;i<n;i++)s+=i*216+217;return s;}
function f216(n){var s=0;for(var i=0;i<n;i++)s+=i*217+218;return s;}
function f217(n){var s=0;for(var i=0;i<n;i++)s+=i*218+219;return s;}
function f218(n){var s=0;for(var i=0;i<n;i++)s+=i*219+220;return s;}
function f219(n){var s=0;for(var i=0;i<n;i++)s+=i*220+221;return s;}
function f220(n){var s=0;for(var i=0;i<n;i++)s+=i*221+222;return s;}
function f221(n){var s=0;for(var i=0;i<n;i++)s+=i*222+223;return s;}
function f222(n){var s=0;for(var i=0;i<n;i++)s+=i*223+224;return s;}
function f223(n){var s=0;for(var i=0;i<n;i++)s+=i*224+225;return s;}
function f224(n){var s=0;for(var i=0;i<n;i++)s+=i*225+226;return s;}
function f225(n){var s=0;for(var i=0;i<n;i++)s+=i*226+227;return s;}
function f226(n){var s=0;for(var i=0;i<n;i++)s+=i*227+228;return s;}
function f227(n){var s=0;for(var i=0;i<n;i++)s+=i*228+229;return s;}
function f228(n){var s=0;for(var i=0;i<n;i++)s+=i*229+230;return s;}
function f229(n){var s=0;for(var i=0;i<n;i++)s+=i*230+231;return s;}
function f230(n){var s=0;for(var i=0;i<n;i++)s+=i*231+232;return s;}
function f231(n){var s=0;for(var i=0;i<n;i++)s+=i*232+233;return s;}
function f232(n){var s=0;for(var i=0;i<n;i++)s+=i*233+234;return s;}
function f233(n){var s=0;for(var i=0;i<n;i++)s+=i*234+235;return s;}
function f234(n){var s=0;for(var i=0;i<n;i++)s+=i*235+236;return s;}
function f235(n){var s=0;for(var i=0;i<n;i++)s+=i*236+237;return s;}
function f236(n){var s=0;for(var i=0;i<n;i++)s+=i*237+238;return s;}
function f237(n){var s=0;for(var i=0;i<n;i++)s+=i*238+239;return s;}
function f238(n){var s=0;for(var i=0;i<n;i++)s+=i*239+240;return s;}
function f239(n){var s=0;for(var i=0;i<n;i++)s+=i*240+241;return s;}
function f240(n){var s=0;for(var i=0;i<n;i++)s+=i*241+242;return s;}
function f241(n){var s=0;for(var i=0;i<n;i++)s+=i*242+243;return s;}
function f242(n){var s=0;for(var i=0;i<n;i++)s+=i*243+244;return s;}
function f243(n){var s=0;for(var i=0;i<n;i++)s+=i*244+245;return s;}
function f244(n){var s=0;for(var i=0;i<n;i++)s+=i*245+246;return s;}
function f245(n){var s=0;for(var i=0;i<n;i++)s+=i*246+247;return s;}
function f246(n){var s=0;for(var i=0;i<n;i++)s+=i*247+248;return s;}
function f247(n){var s=0;for(var i=0;i<n;i++)s+=i*248+249;return s;}
function f248(n){var s=0;for(var i=0;i<n;i++)s+=i*249+250;return s;}
function f249(n){var s=0;for(var i=0;i<n;i++)s+=i*250+251;return s;}
function f250(n){var s=0;for(var i=0;i<n;i++)s+=i*251+252;return s;}
function f251(n){var s=0;for(var i=0;i<n;i++)s+=i*252+253;return s;}
function f252(n){var s=0;for(var i=0;i<n;i++)s+=i*253+254;return s;}
function f253(n){var s=0;for(var i=0;i<n;i++)s+=i*254+255;return s;}
function f254(n){var s=0;for(var i=0;i<n;i++)s+=i*255+256;return s;}
function f255(n){var s=0;for(var i=0;i<n;i++)s+=i*256+257;return s;}
function f256(n){var s=0;for(var i=0;i<n;i++)s+=i*257+258;return s;}
function f257(n){var s=0;for(var i=0;i<n;i++)s+=i*258+259;return s;}
function f258(n){var s=0;for(var i=0;i<n;i++)s+=i*259+260;return s;}
function f259(n){var s=0;for(var i=0;i<n;i++)s+=i*260+261;return s;}
function f260(n){var s=0;for(var i=0;i<n;i++)s+=i*261+262;return s;}
function f261(n){var s=0;for(var i=0;i<n;i++)s+=i*262+263;return s;}
function f262(n){var s=0;for(var i=0;i<n;i++)s+=i*263+264;return s;}
function f263(n){var s=0;for(var i=0;i<n;i++)s+=i*264+265;return s;}
function f264(n){var s=0;for(var i=0;i<n;i++)s+=i*265+266;return s;}
function f265(n){var s=0;for(var i=0;i<n;i++)s+=i*266+267;return s;}
function f266(n){var s=0;for(var i=0;i<n;i++)s+=i*267+268;return s;}
function f267(n){var s=0;for(var i=0;i<n;i++)s+=i*268+269;return s;}
function f268(n){var s=0;for(var i=0;i<n;i++)s+=i*269+270;return s;}
function f269(n){var s=0;for(var i=0;i<n;i++)s+=i*270+271;return s;}
function f270(n){var s=0;for(var i=0;i<n;i++)s+=i*271+272;return s;}
function f271(n){var s=0;for(var i=0;i<n;i++)s+=i*272+273;return s;}
function f272(n){var s=0;for(var i=0;i<n;i++)s+=i*273+274;return s;}
function f273(n){var s=0;for(var i=0;i<n;i++)s+=i*274+275;return s;}
function f274(n){var s=0;for(var i=0;i<n;i++)s+=i*275+276;return s;}
function f275(n){var s=0;for(var i=0;i<n;i++)s+=i*276+277;return s;}
function f276(n){var s=0;for(var i=0;i<n;i++)s+=i*277+278;return s;}
function f277(n){var s=0;for(var i=0;i<n;i++)s+=i*278+279;return s;}
function f278(n){var s=0;for(var i=0;i<n;i++)s+=i*279+280;return s;}
function f279(n){var s=0;for(var i=0;i<n;i++)s+=i*280+281;return s;}
function f280(n){var s=0;for(var i=0;i<n;i++)s+=i*281+282;return s;}
function f281(n){var s=0;for(var i=0;i<n;i++)s+=i*282+283;return s;}
function f282(n){var s=0;for(var i=0;i<n;i++)s+=i*283+284;return s;}
function f283(n){var s=0;for(var i=0;i<n;i++)s+=i*284+285;return s;}
function f284(n){var s=0;for(var i=0;i<n;i++)s+=i*285+286;return s;}
function f285(n){var s=0;for(var i=0;i<n;i++)s+=i*286+287;return s;}
function f286(n){var s=0;for(var i=0;i<n;i++)s+=i*287+288;return s;}
function f287(n){var s=0;for(var i=0;i<n;i++)s+=i*288+289;return s;}
function f288(n){var s=0;for(var i=0;i<n;i++)s+=i*289+290;return s;}
function f289(n){var s=0;for(var i=0;i<n;i++)s+=i*290+291;return s;}
function f290(n){var s=0;for(var i=0;i<n;i++)s+=i*291+292;return s;}
function f291(n){var s=0;for(var i=0;i<n;i++)s+=i*292+293;return s;}
function f292(n){var s=0;for(var i=0;i<n;i++)s+=i*293+294;return s;}
function f293(n){var s=0;for(var i=0;i<n;i++)s+=i*294+295;return s;}
function f294(n){var s=0;for(var i=0;i<n;i++)s+=i*295+296;return s;}
function f295(n){var s=0;for(var i=0;i<n;i++)s+=i*296+297;return s;}
function f296(n){var s=0;for(var i=0;i<n;i++)s+=i*297+298;return s;}
function f297(n){var s=0;for(var i=0;i<n;i++)s+=i*298+299;return s;}
function f298(n){var s=0;for(var i=0;i<n;i++)s+=i*299+300;return s;}
function f299(n){var s=0;for(var i=0;i<n;i++)s+=i*300+301;return s;}
function f300(n){var s=0;for(var i=0;i<n;i++)s+=i*301+302;return s;}
function f301(n){var s=0;for(var i=0;i<n;i++)s+=i*302+303;return s;}
function f302(n){var s=0;for(var i=0;i<n;i++)s+=i*303+304;return s;}
function f303(n){var s=0;for(var i=0;i<n;i++)s+=i*304+305;return s;}
function f304(n){var s=0;for(var i=0;i<n;i++)s+=i*305+306;return s;}
function f305(n){var s=0;for(var i=0;i<n;i++)s+=i*306+307;return s;}
function f306(n){var s=0;for(var i=0;i<n;i++)s+=i*307+308;return s;}
function f307(n){var s=0;for(var i=0;i<n;i++)s+=i*308+309;return s;}
function f308(n){var s=0;for(var i=0;i<n;i++)s+=i*309+310;return s;}
function f309(n){var s=0;for(var i=0;i<n;i++)s+=i*310+311;return s;}
function f310(n){var s=0;for(var i=0;i<n;i++)s+=i*311+312;return s;}
function f311(n){var s=0;for(var i=0;i<n;i++)s+=i*312+313;return s;}
function f312(n){var s=0;for(var i=0;i<n;i++)s+=i*313+314;return s;}
function f313(n){var s=0;for(var i=0;i<n;i++)s+=i*314+315;return s;}
function f314(n){var s=0;for(var i=0;i<n;i++)s+=i*315+316;return s;}
function f315(n){var s=0;for(var i=0;i<n;i++)s+=i*316+317;return s;}
function f316(n){var s=0;for(var i=0;i<n;i++)s+=i*317+318;return s;}
function f317(n){var s=0;for(var i=0;i<n;i++)s+=i*318+319;return s;}
function f318(n){var s=0;for(var i=0;i<n;i++)s+=i*319+320;return s;}
function f319(n){var s=0;for(var i=0;i<n;i++)s+=i*320+321;return s;}
function f320(n){var s=0;for(var i=0;i<n;i++)s+=i*321+322;return s;}
function f321(n){var s=0;for(var i=0;i<n;i++)s+=i*322+323;return s;}
function f322(n){var s=0;for(var i=0;i<n;i++)s+=i*323+324;return s;}
function f323(n){var s=0;for(var i=0;i<n;i++)s+=i*324+325;return s;}
function f324(n){var s=0;for(var i=0;i<n;i++)s+=i*325+326;return s;}
function f325(n){var s=0;for(var i=0;i<n;i++)s+=i*326+327;return s;}
function f326(n){var s=0;for(var i=0;i<n;i++)s+=i*327+328;return s;}
function f327(n){var s=0;for(var i=0;i<n;i++)s+=i*328+329;return s;}
function f328(n){var s=0;for(var i=0;i<n;i++)s+=i*329+330;return s;}
function f329(n){var s=0;for(var i=0;i<n;i++)s+=i*330+331;return s;}
function f330(n){var s=0;for(var i=0;i<n;i++)s+=i*331+332;return s;}
function f331(n){var s=0;for(var i=0;i<n;i++)s+=i*332+333;return s;}
function f332(n){var s=0;for(var i=0;i<n;i++)s+=i*333+334;return s;}
function f333(n){var s=0;for(var i=0;i<n;i++)s+=i*334+335;return s;}
function f334(n){var s=0;for(var i=0;i<n;i++)s+=i*335+336;return s;}
function f335(n){var s=0;for(var i=0;i<n;i++)s+=i*336+337;return s;}
function f336(n){var s=0;for(var i=0;i<n;i++)s+=i*337+338;return s;}
function f337(n){var s=0;for(var i=0;i<n;i++)s+=i*338+339;return s;}
function f338(n){var s=0;for(var i=0;i<n;i++)s+=i*339+340;return s;}
function f339(n){var s=0;for(var i=0;i<n;i++)s+=i*340+341;return s;}
function f340(n){var s=0;for(var i=0;i<n;i++)s+=i*341+342;return s;}
function f341(n){var s=0;for(var i=0;i<n;i++)s+=i*342+343;return s;}
function f342(n){var s=0;for(var i=0;i<n;i++)s+=i*343+344;return s;}
function f343(n){var s=0;for(var i=0;i<n;i++)s+=i*344+345;return s;}
function f344(n){var s=0;for(var i=0;i<n;i++)s+=i*345+346;return s;}
function f345(n){var s=0;for(var i=0;i<n;i++)s+=i*346+347;return s;}
function f346(n){var s=0;for(var i=0;i<n;i++)s+=i*347+348;return s;}
function f347(n){var s=0;for(var i=0;i<n;i++)s+=i*348+349;return s;}
function f348(n){var s=0;for(var i=0;i<n;i++)s+=i*349+350;return s;}
function f349(n){var s=0;for(var i=0;i<n;i++)s+=i*350+351;return s;}
function f350(n){var s=0;for(var i=0;i<n;i++)s+=i*351+352;return s;}
function f351(n){var s=0;for(var i=0;i<n;i++)s+=i*352+353;return s;}
function f352(n){var s=0;for(var i=0;i<n;i++)s+=i*353+354;return s;}
function f353(n){var s=0;for(var i=0;i<n;i++)s+=i*354+355;return s;}
function f354(n){var s=0;for(var i=0;i<n;i++)s+=i*355+356;return s;}
function f355(n){var s=0;for(var i=0;i<n;i++)s+=i*356+357;return s;}
function f356(n){var s=0;for(var i=0;i<n;i++)s+=i*357+358;return s;}
function f357(n){var s=0;for(var i=0;i<n;i++)s+=i*358+359;return s;}
function f358(n){var s=0;for(var i=0;i<n;i++)s+=i*359+360;return s;}
function f359(n){var s=0;for(var i=0;i<n;i++)s+=i*360+361;return s;}
function f360(n){var s=0;for(var i=0;i<n;i++)s+=i*361+362;return s;}
function f361(n){var s=0;for(var i=0;i<n;i++)s+=i*362+363;return s;}
function f362(n){var s=0;for(var i=0;i<n;i++)s+=i*363+364;return s;}
function f363(n){var s=0;for(var i=0;i<n;i++)s+=i*364+365;return s;}
function f364(n){var s=0;for(var i=0;i<n;i++)s+=i*365+366;return s;}
function f365(n){var s=0;for(var i=0;i<n;i++)s+=i*366+367;return s;}
function f366(n){var s=0;for(var i=0;i<n;i++)s+=i*367+368;return s;}
function f367(n){var s=0;for(var i=0;i<n;i++)s+=i*368+369;return s;}
function f368(n){var s=0;for(var i=0;i<n;i++)s+=i*369+370;return s;}
function f369(n){var s=0;for(var i=0;i<n;i++)s+=i*370+371;return s;}
function f370(n){var s=0;for(var i=0;i<n;i++)s+=i*371+372;return s;}
function f371(n){var s=0;for(var i=0;i<n;i++)s+=i*372+373;return s;}
function f372(n){var s=0;for(var i=0;i<n;i++)s+=i*373+374;return s;}
function f373(n){var s=0;for(var i=0;i<n;i++)s+=i*374+375;return s;}
function f374(n){var s=0;for(var i=0;i<n;i++)s+=i*375+376;return s;}
function f375(n){var s=0;for(var i=0;i<n;i++)s+=i*376+377;return s;}
function f376(n){var s=0;for(var i=0;i<n;i++)s+=i*377+378;return s;}
function f377(n){var s=0;for(var i=0;i<n;i++)s+=i*378+379;return s;}
function f378(n){var s=0;for(var i=0;i<n;i++)s+=i*379+380;return s;}
function f379(n){var s=0;for(var i=0;i<n;i++)s+=i*380+381;return s;}
function f380(n){var s=0;for(var i=0;i<n;i++)s+=i*381+382;return s;}
function f381(n){var s=0;for(var i=0;i<n;i++)s+=i*382+383;return s;}
function f382(n){var s=0;for(var i=0;i<n;i++)s+=i*383+384;return s;}
function f383(n){var s=0;for(var i=0;i<n;i++)s+=i*384+385;return s;}
function f384(n){var s=0;for(var i=0;i<n;i++)s+=i*385+386;return s;}
function f385(n){var s=0;for(var i=0;i<n;i++)s+=i*386+387;return s;}
function f386(n){var s=0;for(var i=0;i<n;i++)s+=i*387+388;return s;}
function f387(n){var s=0;for(var i=0;i<n;i++)s+=i*388+389;return s;}
function f388(n){var s=0;for(var i=0;i<n;i++)s+=i*389+390;return s;}
function f389(n){var s=0;for(var i=0;i<n;i++)s+=i*390+391;return s;}
function f390(n){var s=0;for(var i=0;i<n;i++)s+=i*391+392;return s;}
function f391(n){var s=0;for(var i=0;i<n;i++)s+=i*392+393;return s;}
function f392(n){var s=0;for(var i=0;i<n;i++)s+=i*393+394;return s;}
function f393(n){var s=0;for(var i=0;i<n;i++)s+=i*394+395;return s;}
function f394(n){var s=0;for(var i=0;i<n;i++)s+=i*395+396;return s;}
function f395(n){var s=0;for(var i=0;i<n;i++)s+=i*396+397;return s;}
function f396(n){var s=0;for(var i=0;i<n;i++)s+=i*397+398;return s;}
function f397(n){var s=0;for(var i=0;i<n;i++)s+=i*398+399;return s;}
function f398(n){var s=0;for(var i=0;i<n;i++)s+=i*399+400;return s;}
function f399(n){var s=0;for(var i=0;i<n;i++)s+=i*400+401;return s;}
function f400(n){var s=0;for(var i=0;i<n;i++)s+=i*401+402;return s;}
function f401(n){var s=0;for(var i=0;i<n;i++)s+=i*402+403;return s;}
function f402(n){var s=0;for(var i=0;i<n;i++)s+=i*403+404;return s;}
function f403(n){var s=0;for(var i=0;i<n;i++)s+=i*404+405;return s;}
function f404(n){var s=0;for(var i=0;i<n;i++)s+=i*405+406;return s;}
function f405(n){var s=0;for(var i=0;i<n;i++)s+=i*406+407;return s;}
function f406(n){var s=0;for(var i=0;i<n;i++)s+=i*407+408;return s;}
function f407(n){var s=0;for(var i=0;i<n;i++)s+=i*408+409;return s;}
function f408(n){var s=0;for(var i=0;i<n;i++)s+=i*409+410;return s;}
function f409(n){var s=0;for(var i=0;i<n;i++)s+=i*410+411;return s;}
function f410(n){var s=0;for(var i=0;i<n;i++)s+=i*411+412;return s;}
function f411(n){var s=0;for(var i=0;i<n;i++)s+=i*412+413;return s;}
function f412(n){var s=0;for(var i=0;i<n;i++)s+=i*413+414;return s;}
function f413(n){var s=0;for(var i=0;i<n;i++)s+=i*414+415;return s;}
function f414(n){var s=0;for(var i=0;i<n;i++)s+=i*415+416;return s;}
function f415(n){var s=0;for(var i=0;i<n;i++)s+=i*416+417;return s;}
function f416(n){var s=0;for(var i=0;i<n;i++)s+=i*417+418;return s;}
function f417(n){var s=0;for(var i=0;i<n;i++)s+=i*418+419;return s;}
function f418(n){var s=0;for(var i=0;i<n;i++)s+=i*419+420;return s;}
function f419(n){var s=0;for(var i=0;i<n;i++)s+=i*420+421;return s;}
function f420(n){var s=0;for(var i=0;i<n;i++)s+=i*421+422;return s;}
function f421(n){var s=0;for(var i=0;i<n;i++)s+=i*422+423;return s;}
function f422(n){var s=0;for(var i=0;i<n;i++)s+=i*423+424;return s;}
function f423(n){var s=0;for(var i=0;i<n;i++)s+=i*424+425;return s;}
function f424(n){var s=0;for(var i=0;i<n;i++)s+=i*425+426;return s;}
function f425(n){var s=0;for(var i=0;i<n;i++)s+=i*426+427;return s;}
function f426(n){var s=0;for(var i=0;i<n;i++)s+=i*427+428;return s;}
function f427(n){var s=0;for(var i=0;i<n;i++)s+=i*428+429;return s;}
function f428(n){var s=0;for(var i=0;i<n;i++)s+=i*429+430;return s;}
function f429(n){var s=0;for(var i=0;i<n;i++)s+=i*430+431;return s;}
function f430(n){var s=0;for(var i=0;i<n;i++)s+=i*431+432;return s;}
function f431(n){var s=0;for(var i=0;i<n;i++)s+=i*432+433;return s;}
function f432(n){var s=0;for(var i=0;i<n;i++)s+=i*433+434;return s;}
function f433(n){var s=0;for(var i=0;i<n;i++)s+=i*434+435;return s;}
function f434(n){var s=0;for(var i=0;i<n;i++)s+=i*435+436;return s;}
function f435(n){var s=0;for(var i=0;i<n;i++)s+=i*436+437;return s;}
function f436(n){var s=0;for(var i=0;i<n;i++)s+=i*437+438;return s;}
function f437(n){var s=0;for(var i=0;i<n;i++)s+=i*438+439;return s;}
function f438(n){var s=0;for(var i=0;i<n;i++)s+=i*439+440;return s;}
function f439(n){var s=0;for(var i=0;i<n;i++)s+=i*440+441;return s;}
function f440(n){var s=0;for(var i=0;i<n;i++)s+=i*441+442;return s;}
function f441(n){var s=0;for(var i=0;i<n;i++)s+=i*442+443;return s;}
function f442(n){var s=0;for(var i=0;i<n;i++)s+=i*443+444;return s;}
function f443(n){var s=0;for(var i=0;i<n;i++)s+=i*444+445;return s;}
function f444(n){var s=0;for(var i=0;i<n;i++)s+=i*445+446;return s;}
function f445(n){var s=0;for(var i=0;i<n;i++)s+=i*446+447;return s;}
function f446(n){var s=0;for(var i=0;i<n;i++)s+=i*447+448;return s;}
function f447(n){var s=0;for(var i=0;i<n;i++)s+=i*448+449;return s;}
function f448(n){var s=0;for(var i=0;i<n;i++)s+=i*449+450;return s;}
function f449(n){var s=0;for(var i=0;i<n;i++)s+=i*450+451;return s;}
function f450(n){var s=0;for(var i=0;i<n;i++)s+=i*451+452;return s;}
function f451(n){var s=0;for(var i=0;i<n;i++)s+=i*452+453;return s;}
function f452(n){var s=0;for(var i=0;i<n;i++)s+=i*453+454;return s;}
function f453(n){var s=0;for(var i=0;i<n;i++)s+=i*454+455;return s;}
function f454(n){var s=0;for(var i=0;i<n;i++)s+=i*455+456;return s;}
function f455(n){var s=0;for(var i=0;i<n;i++)s+=i*456+457;return s;}
function f456(n){var s=0;for(var i=0;i<n;i++)s+=i*457+458;return s;}
function f457(n){var s=0;for(var i=0;i<n;i++)s+=i*458+459;return s;}
function f458(n){var s=0;for(var i=0;i<n;i++)s+=i*459+460;return s;}
function f459(n){var s=0;for(var i=0;i<n;i++)s+=i*460+461;return s;}
function f460(n){var s=0;for(var i=0;i<n;i++)s+=i*461+462;return s;}
function f461(n){var s=0;for(var i=0;i<n;i++)s+=i*462+463;return s;}
function f462(n){var s=0;for(var i=0;i<n;i++)s+=i*463+464;return s;}
function f463(n){var s=0;for(var i=0;i<n;i++)s+=i*464+465;return s;}
function f464(n){var s=0;for(var i=0;i<n;i++)s+=i*465+466;return s;}
function f465(n){var s=0;for(var i=0;i<n;i++)s+=i*466+467;return s;}
function f466(n){var s=0;for(var i=0;i<n;i++)s+=i*467+468;return s;}
function f467(n){var s=0;for(var i=0;i<n;i++)s+=i*468+469;return s;}
function f468(n){var s=0;for(var i=0;i<n;i++)s+=i*469+470;return s;}
function f469(n){var s=0;for(var i=0;i<n;i++)s+=i*470+471;return s;}
function f470(n){var s=0;for(var i=0;i<n;i++)s+=i*471+472;return s;}
function f471(n){var s=0;for(var i=0;i<n;i++)s+=i*472+473;return s;}
function f472(n){var s=0;for(var i=0;i<n;i++)s+=i*473+474;return s;}
function f473(n){var s=0;for(var i=0;i<n;i++)s+=i*474+475;return s;}
function f474(n){var s=0;for(var i=0;i<n;i++)s+=i*475+476;return s;}
function f475(n){var s=0;for(var i=0;i<n;i++)s+=i*476+477;return s;}
function f476(n){var s=0;for(var i=0;i<n;i++)s+=i*477+478;return s;}
function f477(n){var s=0;for(var i=0;i<n;i++)s+=i*478+479;return s;}
function f478(n){var s=0;for(var i=0;i<n;i++)s+=i*479+480;return s;}
function f479(n){var s=0;for(var i=0;i<n;i++)s+=i*480+481;return s;}
function f480(n){var s=0;for(var i=0;i<n;i++)s+=i*481+482;return s;}
function f481(n){var s=0;for(var i=0;i<n;i++)s+=i*482+483;return s;}
function f482(n){var s=0;for(var i=0;i<n;i++)s+=i*483+484;return s;}
function f483(n){var s=0;for(var i=0;i<n;i++)s+=i*484+485;return s;}
function f484(n){var s=0;for(var i=0;i<n;i++)s+=i*485+486;return s;}
function f485(n){var s=0;for(var i=0;i<n;i++)s+=i*486+487;return s;}
function f486(n){var s=0;for(var i=0;i<n;i++)s+=i*487+488;return s;}
function f487(n){var s=0;for(var i=0;i<n;i++)s+=i*488+489;return s;}
function f488(n){var s=0;for(var i=0;i<n;i++)s+=i*489+490;return s;}
function f489(n){var s=0;for(var i=0;i<n;i++)s+=i*490+491;return s;}
function f490(n){var s=0;for(var i=0;i<n;i++)s+=i*491+492;return s;}
function f491(n){var s=0;for(var i=0;i<n;i++)s+=i*492+493;return s;}
function f492(n){var s=0;for(var i=0;i<n;i++)s+=i*493+494;return s;}
function f493(n){var s=0;for(var i=0;i<n;i++)s+=i*494+495;return s;}
function f494(n){var s=0;for(var i=0;i<n;i++)s+=i*495+496;return s;}
function f495(n){var s=0;for(var i=0;i<n;i++)s+=i*496+497;return s;}
function f496(n){var s=0;for(var i=0;i<n;i++)s+=i*497+498;return s;}
function f497(n){var s=0;for(var i=0;i<n;i++)s+=i*498+499;return s;}
function f498(n){var s=0;for(var i=0;i<n;i++)s+=i*499+500;return s;}
function f499(n){var s=0;for(var i=0;i<n;i++)s+=i*500+501;return s;}
function f500(n){var s=0;for(var i=0;i<n;i++)s+=i*501+502;return s;}
function f501(n){var s=0;for(var i=0;i<n;i++)s+=i*502+503;return s;}
function f502(n){var s=0;for(var i=0;i<n;i++)s+=i*503+504;return s;}
function f503(n){var s=0;for(var i=0;i<n;i++)s+=i*504+505;return s;}
function f504(n){var s=0;for(var i=0;i<n;i++)s+=i*505+506;return s;}
function f505(n){var s=0;for(var i=0;i<n;i++)s+=i*506+507;return s;}
function f506(n){var s=0;for(var i=0;i<n;i++)s+=i*507+508;return s;}
function f507(n){var s=0;for(var i=0;i<n;i++)s+=i*508+509;return s;}
function f508(n){var s=0;for(var i=0;i<n;i++)s+=i*509+510;return s;}
function f509(n){var s=0;for(var i=0;i<n;i++)s+=i*510+511;return s;}
function f510(n){var s=0;for(var i=0;i<n;i++)s+=i*511+512;return s;}
function f511(n){var s=0;for(var i=0;i<n;i++)s+=i*512+513;return s;}
function f512(n){var s=0;for(var i=0;i<n;i++)s+=i*513+514;return s;}
function f513(n){var s=0;for(var i=0;i<n;i++)s+=i*514+515;return s;}
function f514(n){var s=0;for(var i=0;i<n;i++)s+=i*515+516;return s;}
function f515(n){var s=0;for(var i=0;i<n;i++)s+=i*516+517;return s;}
function f516(n){var s=0;for(var i=0;i<n;i++)s+=i*517+518;return s;}
function f517(n){var s=0;for(var i=0;i<n;i++)s+=i*518+519;return s;}
function f518(n){var s=0;for(var i=0;i<n;i++)s+=i*519+520;return s;}
function f519(n){var s=0;for(var i=0;i<n;i++)s+=i*520+521;return s;}
function f520(n){var s=0;for(var i=0;i<n;i++)s+=i*521+522;return s;}
function f521(n){var s=0;for(var i=0;i<n;i++)s+=i*522+523;return s;}
function f522(n){var s=0;for(var i=0;i<n;i++)s+=i*523+524;return s;}
function f523(n){var s=0;for(var i=0;i<n;i++)s+=i*524+525;return s;}
function f524(n){var s=0;for(var i=0;i<n;i++)s+=i*525+526;return s;}
function f525(n){var s=0;for(var i=0;i<n;i++)s+=i*526+527;return s;}
function f526(n){var s=0;for(var i=0;i<n;i++)s+=i*527+528;return s;}
function f527(n){var s=0;for(var i=0;i<n;i++)s+=i*528+529;return s;}
function f528(n){var s=0;for(var i=0;i<n;i++)s+=i*529+530;return s;}
function f529(n){var s=0;for(var i=0;i<n;i++)s+=i*530+531;return s;}
function f530(n){var s=0;for(var i=0;i<n;i++)s+=i*531+532;return s;}
function f531(n){var s=0;for(var i=0;i<n;i++)s+=i*532+533;return s;}
function f532(n){var s=0;for(var i=0;i<n;i++)s+=i*533+534;return s;}
function f533(n){var s=0;for(var i=0;i<n;i++)s+=i*534+535;return s;}
function f534(n){var s=0;for(var i=0;i<n;i++)s+=i*535+536;return s;}
function f535(n){var s=0;for(var i=0;i<n;i++)s+=i*536+537;return s;}
function f536(n){var s=0;for(var i=0;i<n;i++)s+=i*537+538;return s;}
function f537(n){var s=0;for(var i=0;i<n;i++)s+=i*538+539;return s;}
function f538(n){var s=0;for(var i=0;i<n;i++)s+=i*539+540;return s;}
function f539(n){var s=0;for(var i=0;i<n;i++)s+=i*540+541;return s;}
function f540(n){var s=0;for(var i=0;i<n;i++)s+=i*541+542;return s;}
function f541(n){var s=0;for(var i=0;i<n;i++)s+=i*542+543;return s;}
function f542(n){var s=0;for(var i=0;i<n;i++)s+=i*543+544;return s;}
function f543(n){var s=0;for(var i=0;i<n;i++)s+=i*544+545;return s;}
function f544(n){var s=0;for(var i=0;i<n;i++)s+=i*545+546;return s;}
function f545(n){var s=0;for(var i=0;i<n;i++)s+=i*546+547;return s;}
function f546(n){var s=0;for(var i=0;i<n;i++)s+=i*547+548;return s;}
function f547(n){var s=0;for(var i=0;i<n;i++)s+=i*548+549;return s;}
function f548(n){var s=0;for(var i=0;i<n;i++)s+=i*549+550;return s;}
function f549(n){var s=0;for(var i=0;i<n;i++)s+=i*550+551;return s;}
function f550(n){var s=0;for(var i=0;i<n;i++)s+=i*551+552;return s;}
function f551(n){var s=0;for(var i=0;i<n;i++)s+=i*552+553;return s;}
function f552(n){var s=0;for(var i=0;i<n;i++)s+=i*553+554;return s;}
function f553(n){var s=0;for(var i=0;i<n;i++)s+=i*554+555;return s;}
function f554(n){var s=0;for(var i=0;i<n;i++)s+=i*555+556;return s;}
function f555(n){var s=0;for(var i=0;i<n;i++)s+=i*556+557;return s;}
function f556(n){var s=0;for(var i=0;i<n;i++)s+=i*557+558;return s;}
function f557(n){var s=0;for(var i=0;i<n;i++)s+=i*558+559;return s;}
function f558(n){var s=0;for(var i=0;i<n;i++)s+=i*559+560;return s;}
function f559(n){var s=0;for(var i=0;i<n;i++)s+=i*560+561;return s;}
function f560(n){var s=0;for(var i=0;i<n;i++)s+=i*561+562;return s;}
function f561(n){var s=0;for(var i=0;i<n;i++)s+=i*562+563;return s;}
function f562(n){var s=0;for(var i=0;i<n;i++)s+=i*563+564;return s;}
function f563(n){var s=0;for(var i=0;i<n;i++)s+=i*564+565;return s;}
function f564(n){var s=0;for(var i=0;i<n;i++)s+=i*565+566;return s;}
function f565(n){var s=0;for(var i=0;i<n;i++)s+=i*566+567;return s;}
function f566(n){var s=0;for(var i=0;i<n;i++)s+=i*567+568;return s;}
function f567(n){var s=0;for(var i=0;i<n;i++)s+=i*568+569;return s;}
function f568(n){var s=0;for(var i=0;i<n;i++)s+=i*569+570;return s;}
function f569(n){var s=0;for(var i=0;i<n;i++)s+=i*570+571;return s;}
function f570(n){var s=0;for(var i=0;i<n;i++)s+=i*571+572;return s;}
function f571(n){var s=0;for(var i=0;i<n;i++)s+=i*572+573;return s;}
function f572(n){var s=0;for(var i=0;i<n;i++)s+=i*573+574;return s;}
function f573(n){var s=0;for(var i=0;i<n;i++)s+=i*574+575;return s;}
function f574(n){var s=0;for(var i=0;i<n;i++)s+=i*575+576;return s;}
function f575(n){var s=0;for(var i=0;i<n;i++)s+=i*576+577;return s;}
function f576(n){var s=0;for(var i=0;i<n;i++)s+=i*577+578;return s;}
function f577(n){var s=0;for(var i=0;i<n;i++)s+=i*578+579;return s;}
function f578(n){var s=0;for(var i=0;i<n;i++)s+=i*579+580;return s;}
function f579(n){var s=0;for(var i=0;i<n;i++)s+=i*580+581;return s;}
function f580(n){var s=0;for(var i=0;i<n;i++)s+=i*581+582;return s;}
function f581(n){var s=0;for(var i=0;i<n;i++)s+=i*582+583;return s;}
function f582(n){var s=0;for(var i=0;i<n;i++)s+=i*583+584;return s;}
function f583(n){var s=0;for(var i=0;i<n;i++)s+=i*584+585;return s;}
function f584(n){var s=0;for(var i=0;i<n;i++)s+=i*585+586;return s;}
function f585(n){var s=0;for(var i=0;i<n;i++)s+=i*586+587;return s;}
function f586(n){var s=0;for(var i=0;i<n;i++)s+=i*587+588;return s;}
function f587(n){var s=0;for(var i=0;i<n;i++)s+=i*588+589;return s;}
function f588(n){var s=0;for(var i=0;i<n;i++)s+=i*589+590;return s;}
function f589(n){var s=0;for(var i=0;i<n;i++)s+=i*590+591;return s;}
function f590(n){var s=0;for(var i=0;i<n;i++)s+=i*591+592;return s;}
function f591(n){var s=0;for(var i=0;i<n;i++)s+=i*592+593;return s;}
function f592(n){var s=0;for(var i=0;i<n;i++)s+=i*593+594;return s;}
function f593(n){var s=0;for(var i=0;i<n;i++)s+=i*594+595;return s;}
function f594(n){var s=0;for(var i=0;i<n;i++)s+=i*595+596;return s;}
function f595(n){var s=0;for(var i=0;i<n;i++)s+=i*596+597;return s;}
function f596(n){var s=0;for(var i=0;i<n;i++)s+=i*597+598;return s;}
function f597(n){var s=0;for(var i=0;i<n;i++)s+=i*598+599;return s;}
function f598(n){var s=0;for(var i=0;i<n;i++)s+=i*599+600;return s;}
function f599(n){var s=0;for(var i=0;i<n;i++)s+=i*600+601;return s;}
function f600(n){var s=0;for(var i=0;i<n;i++)s+=i*601+602;return s;}
function f601(n){var s=0;for(var i=0;i<n;i++)s+=i*602+603;return s;}
function f602(n){var s=0;for(var i=0;i<n;i++)s+=i*603+604;return s;}
function f603(n){var s=0;for(var i=0;i<n;i++)s+=i*604+605;return s;}
function f604(n){var s=0;for(var i=0;i<n;i++)s+=i*605+606;return s;}
function f605(n){var s=0;for(var i=0;i<n;i++)s+=i*606+607;return s;}
function f606(n){var s=0;for(var i=0;i<n;i++)s+=i*607+608;return s;}
function f607(n){var s=0;for(var i=0;i<n;i++)s+=i*608+609;return s;}
function f608(n){var s=0;for(var i=0;i<n;i++)s+=i*609+610;return s;}
function f609(n){var s=0;for(var i=0;i<n;i++)s+=i*610+611;return s;}
function f610(n){var s=0;for(var i=0;i<n;i++)s+=i*611+612;return s;}
function f611(n){var s=0;for(var i=0;i<n;i++)s+=i*612+613;return s;}
function f612(n){var s=0;for(var i=0;i<n;i++)s+=i*613+614;return s;}
function f613(n){var s=0;for(var i=0;i<n;i++)s+=i*614+615;return s;}
function f614(n){var s=0;for(var i=0;i<n;i++)s+=i*615+616;return s;}
function f615(n){var s=0;for(var i=0;i<n;i++)s+=i*616+617;return s;}
function f616(n){var s=0;for(var i=0;i<n;i++)s+=i*617+618;return s;}
function f617(n){var s=0;for(var i=0;i<n;i++)s+=i*618+619;return s;}
function f618(n){var s=0;for(var i=0;i<n;i++)s+=i*619+620;return s;}
function f619(n){var s=0;for(var i=0;i<n;i++)s+=i*620+621;return s;}
function f620(n){var s=0;for(var i=0;i<n;i++)s+=i*621+622;return s;}
function f621(n){var s=0;for(var i=0;i<n;i++)s+=i*622+623;return s;}
function f622(n){var s=0;for(var i=0;i<n;i++)s+=i*623+624;return s;}
function f623(n){var s=0;for(var i=0;i<n;i++)s+=i*624+625;return s;}
function f624(n){var s=0;for(var i=0;i<n;i++)s+=i*625+626;return s;}
function f625(n){var s=0;for(var i=0;i<n;i++)s+=i*626+627;return s;}
function f626(n){var s=0;for(var i=0;i<n;i++)s+=i*627+628;return s;}
function f627(n){var s=0;for(var i=0;i<n;i++)s+=i*628+629;return s;}
function f628(n){var s=0;for(var i=0;i<n;i++)s+=i*629+630;return s;}
function f629(n){var s=0;for(var i=0;i<n;i++)s+=i*630+631;return s;}
function f630(n){var s=0;for(var i=0;i<n;i++)s+=i*631+632;return s;}
function f631(n){var s=0;for(var i=0;i<n;i++)s+=i*632+633;return s;}
function f632(n){var s=0;for(var i=0;i<n;i++)s+=i*633+634;return s;}
function f633(n){var s=0;for(var i=0;i<n;i++)s+=i*634+635;return s;}
function f634(n){var s=0;for(var i=0;i<n;i++)s+=i*635+636;return s;}
function f635(n){var s=0;for(var i=0;i<n;i++)s+=i*636+637;return s;}
function f636(n){var s=0;for(var i=0;i<n;i++)s+=i*637+638;return s;}
function f637(n){var s=0;for(var i=0;i<n;i++)s+=i*638+639;return s;}
function f638(n){var s=0;for(var i=0;i<n;i++)s+=i*639+640;return s;}
function f639(n){var s=0;for(var i=0;i<n;i++)s+=i*640+641;return s;}
function f640(n){var s=0;for(var i=0;i<n;i++)s+=i*641+642;return s;}
function f641(n){var s=0;for(var i=0;i<n;i++)s+=i*642+643;return s;}
function f642(n){var s=0;for(var i=0;i<n;i++)s+=i*643+644;return s;}
function f643(n){var s=0;for(var i=0;i<n;i++)s+=i*644+645;return s;}
function f644(n){var s=0;for(var i=0;i<n;i++)s+=i*645+646;return s;}
function f645(n){var s=0;for(var i=0;i<n;i++)s+=i*646+647;return s;}
function f646(n){var s=0;for(var i=0;i<n;i++)s+=i*647+648;return s;}
function f647(n){var s=0;for(var i=0;i<n;i++)s+=i*648+649;return s;}
function f648(n){var s=0;for(var i=0;i<n;i++)s+=i*649+650;return s;}
function f649(n){var s=0;for(var i=0;i<n;i++)s+=i*650+651;return s;}
function f650(n){var s=0;for(var i=0;i<n;i++)s+=i*651+652;return s;}
function f651(n){var s=0;for(var i=0;i<n;i++)s+=i*652+653;return s;}
function f652(n){var s=0;for(var i=0;i<n;i++)s+=i*653+654;return s;}
function f653(n){var s=0;for(var i=0;i<n;i++)s+=i*654+655;return s;}
function f654(n){var s=0;for(var i=0;i<n;i++)s+=i*655+656;return s;}
function f655(n){var s=0;for(var i=0;i<n;i++)s+=i*656+657;return s;}
function f656(n){var s=0;for(var i=0;i<n;i++)s+=i*657+658;return s;}
function f657(n){var s=0;for(var i=0;i<n;i++)s+=i*658+659;return s;}
function f658(n){var s=0;for(var i=0;i<n;i++)s+=i*659+660;return s;}
function f659(n){var s=0;for(var i=0;i<n;i++)s+=i*660+661;return s;}
function f660(n){var s=0;for(var i=0;i<n;i++)s+=i*661+662;return s;}
function f661(n){var s=0;for(var i=0;i<n;i++)s+=i*662+663;return s;}
function f662(n){var s=0;for(var i=0;i<n;i++)s+=i*663+664;return s;}
function f663(n){var s=0;for(var i=0;i<n;i++)s+=i*664+665;return s;}
function f664(n){var s=0;for(var i=0;i<n;i++)s+=i*665+666;return s;}
function f665(n){var s=0;for(var i=0;i<n;i++)s+=i*666+667;return s;}
function f666(n){var s=0;for(var i=0;i<n;i++)s+=i*667+668;return s;}
function f667(n){var s=0;for(var i=0;i<n;i++)s+=i*668+669;return s;}
function f668(n){var s=0;for(var i=0;i<n;i++)s+=i*669+670;return s;}
function f669(n){var s=0;for(var i=0;i<n;i++)s+=i*670+671;return s;}
function f670(n){var s=0;for(var i=0;i<n;i++)s+=i*671+672;return s;}
function f671(n){var s=0;for(var i=0;i<n;i++)s+=i*672+673;return s;}
function f672(n){var s=0;for(var i=0;i<n;i++)s+=i*673+674;return s;}
function f673(n){var s=0;for(var i=0;i<n;i++)s+=i*674+675;return s;}
function f674(n){var s=0;for(var i=0;i<n;i++)s+=i*675+676;return s;}
function f675(n){var s=0;for(var i=0;i<n;i++)s+=i*676+677;return s;}
function f676(n){var s=0;for(var i=0;i<n;i++)s+=i*677+678;return s;}
function f677(n){var s=0;for(var i=0;i<n;i++)s+=i*678+679;return s;}
function f678(n){var s=0;for(var i=0;i<n;i++)s+=i*679+680;return s;}
function f679(n){var s=0;for(var i=0;i<n;i++)s+=i*680+681;return s;}
function f680(n){var s=0;for(var i=0;i<n;i++)s+=i*681+682;return s;}
function f681(n){var s=0;for(var i=0;i<n;i++)s+=i*682+683;return s;}
function f682(n){var s=0;for(var i=0;i<n;i++)s+=i*683+684;return s;}
function f683(n){var s=0;for(var i=0;i<n;i++)s+=i*684+685;return s;}
function f684(n){var s=0;for(var i=0;i<n;i++)s+=i*685+686;return s;}
function f685(n){var s=0;for(var i=0;i<n;i++)s+=i*686+687;return s;}
function f686(n){var s=0;for(var i=0;i<n;i++)s+=i*687+688;return s;}
function f687(n){var s=0;for(var i=0;i<n;i++)s+=i*688+689;return s;}
function f688(n){var s=0;for(var i=0;i<n;i++)s+=i*689+690;return s;}
function f689(n){var s=0;for(var i=0;i<n;i++)s+=i*690+691;return s;}
function f690(n){var s=0;for(var i=0;i<n;i++)s+=i*691+692;return s;}
function f691(n){var s=0;for(var i=0;i<n;i++)s+=i*692+693;return s;}
function f692(n){var s=0;for(var i=0;i<n;i++)s+=i*693+694;return s;}
function f693(n){var s=0;for(var i=0;i<n;i++)s+=i*694+695;return s;}
function f694(n){var s=0;for(var i=0;i<n;i++)s+=i*695+696;return s;}
function f695(n){var s=0;for(var i=0;i<n;i++)s+=i*696+697;return s;}
function f696(n){var s=0;for(var i=0;i<n;i++)s+=i*697+698;return s;}
function f697(n){var s=0;for(var i=0;i<n;i++)s+=i*698+699;return s;}
function f698(n){var s=0;for(var i=0;i<n;i++)s+=i*699+700;return s;}
function f699(n){var s=0;for(var i=0;i<n;i++)s+=i*700+701;return s;}
function f700(n){var s=0;for(var i=0;i<n;i++)s+=i*701+702;return s;}
function f701(n){var s=0;for(var i=0;i<n;i++)s+=i*702+703;return s;}
function f702(n){var s=0;for(var i=0;i<n;i++)s+=i*703+704;return s;}
function f703(n){var s=0;for(var i=0;i<n;i++)s+=i*704+705;return s;}
function f704(n){var s=0;for(var i=0;i<n;i++)s+=i*705+706;return s;}
function f705(n){var s=0;for(var i=0;i<n;i++)s+=i*706+707;return s;}
function f706(n){var s=0;for(var i=0;i<n;i++)s+=i*707+708;return s;}
function f707(n){var s=0;for(var i=0;i<n;i++)s+=i*708+709;return s;}
function f708(n){var s=0;for(var i=0;i<n;i++)s+=i*709+710;return s;}
function f709(n){var s=0;for(var i=0;i<n;i++)s+=i*710+711;return s;}
function f710(n){var s=0;for(var i=0;i<n;i++)s+=i*711+712;return s;}
function f711(n){var s=0;for(var i=0;i<n;i++)s+=i*712+713;return s;}
function f712(n){var s=0;for(var i=0;i<n;i++)s+=i*713+714;return s;}
function f713(n){var s=0;for(var i=0;i<n;i++)s+=i*714+715;return s;}
function f714(n){var s=0;for(var i=0;i<n;i++)s+=i*715+716;return s;}
function f715(n){var s=0;for(var i=0;i<n;i++)s+=i*716+717;return s;}
function f716(n){var s=0;for(var i=0;i<n;i++)s+=i*717+718;return s;}
function f717(n){var s=0;for(var i=0;i<n;i++)s+=i*718+719;return s;}
function f718(n){var s=0;for(var i=0;i<n;i++)s+=i*719+720;return s;}
function f719(n){var s=0;for(var i=0;i<n;i++)s+=i*720+721;return s;}
function f720(n){var s=0;for(var i=0;i<n;i++)s+=i*721+722;return s;}
function f721(n){var s=0;for(var i=0;i<n;i++)s+=i*722+723;return s;}
function f722(n){var s=0;for(var i=0;i<n;i++)s+=i*723+724;return s;}
function f723(n){var s=0;for(var i=0;i<n;i++)s+=i*724+725;return s;}
function f724(n){var s=0;for(var i=0;i<n;i++)s+=i*725+726;return s;}
function f725(n){var s=0;for(var i=0;i<n;i++)s+=i*726+727;return s;}
function f726(n){var s=0;for(var i=0;i<n;i++)s+=i*727+728;return s;}
function f727(n){var s=0;for(var i=0;i<n;i++)s+=i*728+729;return s;}
function f728(n){var s=0;for(var i=0;i<n;i++)s+=i*729+730;return s;}
function f729(n){var s=0;for(var i=0;i<n;i++)s+=i*730+731;return s;}
function f730(n){var s=0;for(var i=0;i<n;i++)s+=i*731+732;return s;}
function f731(n){var s=0;for(var i=0;i<n;i++)s+=i*732+733;return s;}
function f732(n){var s=0;for(var i=0;i<n;i++)s+=i*733+734;return s;}
function f733(n){var s=0;for(var i=0;i<n;i++)s+=i*734+735;return s;}
function f734(n){var s=0;for(var i=0;i<n;i++)s+=i*735+736;return s;}
function f735(n){var s=0;for(var i=0;i<n;i++)s+=i*736+737;return s;}
function f736(n){var s=0;for(var i=0;i<n;i++)s+=i*737+738;return s;}
function f737(n){var s=0;for(var i=0;i<n;i++)s+=i*738+739;return s;}
function f738(n){var s=0;for(var i=0;i<n;i++)s+=i*739+740;return s;}
function f739(n){var s=0;for(var i=0;i<n;i++)s+=i*740+741;return s;}
function f740(n){var s=0;for(var i=0;i<n;i++)s+=i*741+742;return s;}
function f741(n){var s=0;for(var i=0;i<n;i++)s+=i*742+743;return s;}
function f742(n){var s=0;for(var i=0;i<n;i++)s+=i*743+744;return s;}
function f743(n){var s=0;for(var i=0;i<n;i++)s+=i*744+745;return s;}
function f744(n){var s=0;for(var i=0;i<n;i++)s+=i*745+746;return s;}
function f745(n){var s=0;for(var i=0;i<n;i++)s+=i*746+747;return s;}
function f746(n){var s=0;for(var i=0;i<n;i++)s+=i*747+748;return s;}
function f747(n){var s=0;for(var i=0;i<n;i++)s+=i*748+749;return s;}
function f748(n){var s=0;for(var i=0;i<n;i++)s+=i*749+750;return s;}
function f749(n){var s=0;for(var i=0;i<n;i++)s+=i*750+751;return s;}
function f750(n){var s=0;for(var i=0;i<n;i++)s+=i*751+752;return s;}
function f751(n){var s=0;for(var i=0;i<n;i++)s+=i*752+753;return s;}
function f752(n){var s=0;for(var i=0;i<n;i++)s+=i*753+754;return s;}
function f753(n){var s=0;for(var i=0;i<n;i++)s+=i*754+755;return s;}
function f754(n){var s=0;for(var i=0;i<n;i++)s+=i*755+756;return s;}
function f755(n){var s=0;for(var i=0;i<n;i++)s+=i*756+757;return s;}
function f756(n){var s=0;for(var i=0;i<n;i++)s+=i*757+758;return s;}
function f757(n){var s=0;for(var i=0;i<n;i++)s+=i*758+759;return s;}
function f758(n){var s=0;for(var i=0;i<n;i++)s+=i*759+760;return s;}
function f759(n){var s=0;for(var i=0;i<n;i++)s+=i*760+761;return s;}
function f760(n){var s=0;for(var i=0;i<n;i++)s+=i*761+762;return s;}
function f761(n){var s=0;for(var i=0;i<n;i++)s+=i*762+763;return s;}
function f762(n){var s=0;for(var i=0;i<n;i++)s+=i*763+764;return s;}
function f763(n){var s=0;for(var i=0;i<n;i++)s+=i*764+765;return s;}
function f764(n){var s=0;for(var i=0;i<n;i++)s+=i*765+766;return s;}
function f765(n){var s=0;for(var i=0;i<n;i++)s+=i*766+767;return s;}
function f766(n){var s=0;for(var i=0;i<n;i++)s+=i*767+768;return s;}
function f767(n){var s=0;for(var i=0;i<n;i++)s+=i*768+769;return s;}
function f768(n){var s=0;for(var i=0;i<n;i++)s+=i*769+770;return s;}
function f769(n){var s=0;for(var i=0;i<n;i++)s+=i*770+771;return s;}
function f770(n){var s=0;for(var i=0;i<n;i++)s+=i*771+772;return s;}
function f771(n){var s=0;for(var i=0;i<n;i++)s+=i*772+773;return s;}
function f772(n){var s=0;for(var i=0;i<n;i++)s+=i*773+774;return s;}
function f773(n){var s=0;for(var i=0;i<n;i++)s+=i*774+775;return s;}
function f774(n){var s=0;for(var i=0;i<n;i++)s+=i*775+776;return s;}
function f775(n){var s=0;for(var i=0;i<n;i++)s+=i*776+777;return s;}
function f776(n){var s=0;for(var i=0;i<n;i++)s+=i*777+778;return s;}
function f777(n){var s=0;for(var i=0;i<n;i++)s+=i*778+779;return s;}
function f778(n){var s=0;for(var i=0;i<n;i++)s+=i*779+780;return s;}
function f779(n){var s=0;for(var i=0;i<n;i++)s+=i*780+781;return s;}
function f780(n){var s=0;for(var i=0;i<n;i++)s+=i*781+782;return s;}
function f781(n){var s=0;for(var i=0;i<n;i++)s+=i*782+783;return s;}
function f782(n){var s=0;for(var i=0;i<n;i++)s+=i*783+784;return s;}
function f783(n){var s=0;for(var i=0;i<n;i++)s+=i*784+785;return s;}
function f784(n){var s=0;for(var i=0;i<n;i++)s+=i*785+786;return s;}
function f785(n){var s=0;for(var i=0;i<n;i++)s+=i*786+787;return s;}
function f786(n){var s=0;for(var i=0;i<n;i++)s+=i*787+788;return s;}
function f787(n){var s=0;for(var i=0;i<n;i++)s+=i*788+789;return s;}
function f788(n){var s=0;for(var i=0;i<n;i++)s+=i*789+790;return s;}
function f789(n){var s=0;for(var i=0;i<n;i++)s+=i*790+791;return s;}
function f790(n){var s=0;for(var i=0;i<n;i++)s+=i*791+792;return s;}
function f791(n){var s=0;for(var i=0;i<n;i++)s+=i*792+793;return s;}
function f792(n){var s=0;for(var i=0;i<n;i++)s+=i*793+794;return s;}
function f793(n){var s=0;for(var i=0;i<n;i++)s+=i*794+795;return s;}
function f794(n){var s=0;for(var i=0;i<n;i++)s+=i*795+796;return s;}
function f795(n){var s=0;for(var i=0;i<n;i++)s+=i*796+797;return s;}
function f796(n){var s=0;for(var i=0;i<n;i++)s+=i*797+798;return s;}
function f797(n){var s=0;for(var i=0;i<n;i++)s+=i*798+799;return s;}
function f798(n){var s=0;for(var i=0;i<n;i++)s+=i*799+800;return s;}
function f799(n){var s=0;for(var i=0;i<n;i++)s+=i*800+801;return s;}
function f800(n){var s=0;for(var i=0;i<n;i++)s+=i*801+802;return s;}
function f801(n){var s=0;for(var i=0;i<n;i++)s+=i*802+803;return s;}
function f802(n){var s=0;for(var i=0;i<n;i++)s+=i*803+804;return s;}
function f803(n){var s=0;for(var i=0;i<n;i++)s+=i*804+805;return s;}
function f804(n){var s=0;for(var i=0;i<n;i++)s+=i*805+806;return s;}
function f805(n){var s=0;for(var i=0;i<n;i++)s+=i*806+807;return s;}
function f806(n){var s=0;for(var i=0;i<n;i++)s+=i*807+808;return s;}
function f807(n){var s=0;for(var i=0;i<n;i++)s+=i*808+809;return s;}
function f808(n){var s=0;for(var i=0;i<n;i++)s+=i*809+810;return s;}
function f809(n){var s=0;for(var i=0;i<n;i++)s+=i*810+811;return s;}
function f810(n){var s=0;for(var i=0;i<n;i++)s+=i*811+812;return s;}
function f811(n){var s=0;for(var i=0;i<n;i++)s+=i*812+813;return s;}
function f812(n){var s=0;for(var i=0;i<n;i++)s+=i*813+814;return s;}
function f813(n){var s=0;for(var i=0;i<n;i++)s+=i*814+815;return s;}
function f814(n){var s=0;for(var i=0;i<n;i++)s+=i*815+816;return s;}
function f815(n){var s=0;for(var i=0;i<n;i++)s+=i*816+817;return s;}
function f816(n){var s=0;for(var i=0;i<n;i++)s+=i*817+818;return s;}
function f817(n){var s=0;for(var i=0;i<n;i++)s+=i*818+819;return s;}
function f818(n){var s=0;for(var i=0;i<n;i++)s+=i*819+820;return s;}
function f819(n){var s=0;for(var i=0;i<n;i++)s+=i*820+821;return s;}
function f820(n){var s=0;for(var i=0;i<n;i++)s+=i*821+822;return s;}
function f821(n){var s=0;for(var i=0;i<n;i++)s+=i*822+823;return s;}
function f822(n){var s=0;for(var i=0;i<n;i++)s+=i*823+824;return s;}
function f823(n){var s=0;for(var i=0;i<n;i++)s+=i*824+825;return s;}
function f824(n){var s=0;for(var i=0;i<n;i++)s+=i*825+826;return s;}
function f825(n){var s=0;for(var i=0;i<n;i++)s+=i*826+827;return s;}
function f826(n){var s=0;for(var i=0;i<n;i++)s+=i*827+828;return s;}
function f827(n){var s=0;for(var i=0;i<n;i++)s+=i*828+829;return s;}
function f828(n){var s=0;for(var i=0;i<n;i++)s+=i*829+830;return s;}
function f829(n){var s=0;for(var i=0;i<n;i++)s+=i*830+831;return s;}
function f830(n){var s=0;for(var i=0;i<n;i++)s+=i*831+832;return s;}
function f831(n){var s=0;for(var i=0;i<n;i++)s+=i*832+833;return s;}
function f832(n){var s=0;for(var i=0;i<n;i++)s+=i*833+834;return s;}
function f833(n){var s=0;for(var i=0;i<n;i++)s+=i*834+835;return s;}
function f834(n){var s=0;for(var i=0;i<n;i++)s+=i*835+836;return s;}
function f835(n){var s=0;for(var i=0;i<n;i++)s+=i*836+837;return s;}
function f836(n){var s=0;for(var i=0;i<n;i++)s+=i*837+838;return s;}
function f837(n){var s=0;for(var i=0;i<n;i++)s+=i*838+839;return s;}
function f838(n){var s=0;for(var i=0;i<n;i++)s+=i*839+840;return s;}
function f839(n){var s=0;for(var i=0;i<n;i++)s+=i*840+841;return s;}
function f840(n){var s=0;for(var i=0;i<n;i++)s+=i*841+842;return s;}
function f841(n){var s=0;for(var i=0;i<n;i++)s+=i*842+843;return s;}
function f842(n){var s=0;for(var i=0;i<n;i++)s+=i*843+844;return s;}
function f843(n){var s=0;for(var i=0;i<n;i++)s+=i*844+845;return s;}
function f844(n){var s=0;for(var i=0;i<n;i++)s+=i*845+846;return s;}
function f845(n){var s=0;for(var i=0;i<n;i++)s+=i*846+847;return s;}
function f846(n){var s=0;for(var i=0;i<n;i++)s+=i*847+848;return s;}
function f847(n){var s=0;for(var i=0;i<n;i++)s+=i*848+849;return s;}
function f848(n){var s=0;for(var i=0;i<n;i++)s+=i*849+850;return s;}
function f849(n){var s=0;for(var i=0;i<n;i++)s+=i*850+851;return s;}
function f850(n){var s=0;for(var i=0;i<n;i++)s+=i*851+852;return s;}
function f851(n){var s=0;for(var i=0;i<n;i++)s+=i*852+853;return s;}
function f852(n){var s=0;for(var i=0;i<n;i++)s+=i*853+854;return s;}
function f853(n){var s=0;for(var i=0;i<n;i++)s+=i*854+855;return s;}
function f854(n){var s=0;for(var i=0;i<n;i++)s+=i*855+856;return s;}
function f855(n){var s=0;for(var i=0;i<n;i++)s+=i*856+857;return s;}
function f856(n){var s=0;for(var i=0;i<n;i++)s+=i*857+858;return s;}
function f857(n){var s=0;for(var i=0;i<n;i++)s+=i*858+859;return s;}
function f858(n){var s=0;for(var i=0;i<n;i++)s+=i*859+860;return s;}
function f859(n){var s=0;for(var i=0;i<n;i++)s+=i*860+861;return s;}
function f860(n){var s=0;for(var i=0;i<n;i++)s+=i*861+862;return s;}
function f861(n){var s=0;for(var i=0;i<n;i++)s+=i*862+863;return s;}
function f862(n){var s=0;for(var i=0;i<n;i++)s+=i*863+864;return s;}
function f863(n){var s=0;for(var i=0;i<n;i++)s+=i*864+865;return s;}
function f864(n){var s=0;for(var i=0;i<n;i++)s+=i*865+866;return s;}
function f865(n){var s=0;for(var i=0;i<n;i++)s+=i*866+867;return s;}
function f866(n){var s=0;for(var i=0;i<n;i++)s+=i*867+868;return s;}
function f867(n){var s=0;for(var i=0;i<n;i++)s+=i*868+869;return s;}
function f868(n){var s=0;for(var i=0;i<n;i++)s+=i*869+870;return s;}
function f869(n){var s=0;for(var i=0;i<n;i++)s+=i*870+871;return s;}
function f870(n){var s=0;for(var i=0;i<n;i++)s+=i*871+872;return s;}
function f871(n){var s=0;for(var i=0;i<n;i++)s+=i*872+873;return s;}
function f872(n){var s=0;for(var i=0;i<n;i++)s+=i*873+874;return s;}
function f873(n){var s=0;for(var i=0;i<n;i++)s+=i*874+875;return s;}
function f874(n){var s=0;for(var i=0;i<n;i++)s+=i*875+876;return s;}
function f875(n){var s=0;for(var i=0;i<n;i++)s+=i*876+877;return s;}
function f876(n){var s=0;for(var i=0;i<n;i++)s+=i*877+878;return s;}
function f877(n){var s=0;for(var i=0;i<n;i++)s+=i*878+879;return s;}
function f878(n){var s=0;for(var i=0;i<n;i++)s+=i*879+880;return s;}
function f879(n){var s=0;for(var i=0;i<n;i++)s+=i*880+881;return s;}
function f880(n){var s=0;for(var i=0;i<n;i++)s+=i*881+882;return s;}
function f881(n){var s=0;for(var i=0;i<n;i++)s+=i*882+883;return s;}
function f882(n){var s=0;for(var i=0;i<n;i++)s+=i*883+884;return s;}
function f883(n){var s=0;for(var i=0;i<n;i++)s+=i*884+885;return s;}
function f884(n){var s=0;for(var i=0;i<n;i++)s+=i*885+886;return s;}
function f885(n){var s=0;for(var i=0;i<n;i++)s+=i*886+887;return s;}
function f886(n){var s=0;for(var i=0;i<n;i++)s+=i*887+888;return s;}
function f887(n){var s=0;for(var i=0;i<n;i++)s+=i*888+889;return s;}
function f888(n){var s=0;for(var i=0;i<n;i++)s+=i*889+890;return s;}
function f889(n){var s=0;for(var i=0;i<n;i++)s+=i*890+891;return s;}
function f890(n){var s=0;for(var i=0;i<n;i++)s+=i*891+892;return s;}
function f891(n){var s=0;for(var i=0;i<n;i++)s+=i*892+893;return s;}
function f892(n){var s=0;for(var i=0;i<n;i++)s+=i*893+894;return s;}
function f893(n){var s=0;for(var i=0;i<n;i++)s+=i*894+895;return s;}
function f894(n){var s=0;for(var i=0;i<n;i++)s+=i*895+896;return s;}
function f895(n){var s=0;for(var i=0;i<n;i++)s+=i*896+897;return s;}
function f896(n){var s=0;for(var i=0;i<n;i++)s+=i*897+898;return s;}
function f897(n){var s=0;for(var i=0;i<n;i++)s+=i*898+899;return s;}
function f898(n){var s=0;for(var i=0;i<n;i++)s+=i*899+900;return s;}
function f899(n){var s=0;for(var i=0;i<n;i++)s+=i*900+901;return s;}
function f900(n){var s=0;for(var i=0;i<n;i++)s+=i*901+902;return s;}
function f901(n){var s=0;for(var i=0;i<n;i++)s+=i*902+903;return s;}
function f902(n){var s=0;for(var i=0;i<n;i++)s+=i*903+904;return s;}
function f903(n){var s=0;for(var i=0;i<n;i++)s+=i*904+905;return s;}
function f904(n){var s=0;for(var i=0;i<n;i++)s+=i*905+906;return s;}
function f905(n){var s=0;for(var i=0;i<n;i++)s+=i*906+907;return s;}
function f906(n){var s=0;for(var i=0;i<n;i++)s+=i*907+908;return s;}
function f907(n){var s=0;for(var i=0;i<n;i++)s+=i*908+909;return s;}
function f908(n){var s=0;for(var i=0;i<n;i++)s+=i*909+910;return s;}
function f909(n){var s=0;for(var i=0;i<n;i++)s+=i*910+911;return s;}
function f910(n){var s=0;for(var i=0;i<n;i++)s+=i*911+912;return s;}
function f911(n){var s=0;for(var i=0;i<n;i++)s+=i*912+913;return s;}
function f912(n){var s=0;for(var i=0;i<n;i++)s+=i*913+914;return s;}
function f913(n){var s=0;for(var i=0;i<n;i++)s+=i*914+915;return s;}
function f914(n){var s=0;for(var i=0;i<n;i++)s+=i*915+916;return s;}
function f915(n){var s=0;for(var i=0;i<n;i++)s+=i*916+917;return s;}
function f916(n){var s=0;for(var i=0;i<n;i++)s+=i*917+918;return s;}
function f917(n){var s=0;for(var i=0;i<n;i++)s+=i*918+919;return s;}
function f918(n){var s=0;for(var i=0;i<n;i++)s+=i*919+920;return s;}
function f919(n){var s=0;for(var i=0;i<n;i++)s+=i*920+921;return s;}
function f920(n){var s=0;for(var i=0;i<n;i++)s+=i*921+922;return s;}
function f921(n){var s=0;for(var i=0;i<n;i++)s+=i*922+923;return s;}
function f922(n){var s=0;for(var i=0;i<n;i++)s+=i*923+924;return s;}
function f923(n){var s=0;for(var i=0;i<n;i++)s+=i*924+925;return s;}
function f924(n){var s=0;for(var i=0;i<n;i++)s+=i*925+926;return s;}
function f925(n){var s=0;for(var i=0;i<n;i++)s+=i*926+927;return s;}
function f926(n){var s=0;for(var i=0;i<n;i++)s+=i*927+928;return s;}
function f927(n){var s=0;for(var i=0;i<n;i++)s+=i*928+929;return s;}
function f928(n){var s=0;for(var i=0;i<n;i++)s+=i*929+930;return s;}
function f929(n){var s=0;for(var i=0;i<n;i++)s+=i*930+931;return s;}
function f930(n){var s=0;for(var i=0;i<n;i++)s+=i*931+932;return s;}
function f931(n){var s=0;for(var i=0;i<n;i++)s+=i*932+933;return s;}
function f932(n){var s=0;for(var i=0;i<n;i++)s+=i*933+934;return s;}
function f933(n){var s=0;for(var i=0;i<n;i++)s+=i*934+935;return s;}
function f934(n){var s=0;for(var i=0;i<n;i++)s+=i*935+936;return s;}
function f935(n){var s=0;for(var i=0;i<n;i++)s+=i*936+937;return s;}
function f936(n){var s=0;for(var i=0;i<n;i++)s+=i*937+938;return s;}
function f937(n){var s=0;for(var i=0;i<n;i++)s+=i*938+939;return s;}
function f938(n){var s=0;for(var i=0;i<n;i++)s+=i*939+940;return s;}
function f939(n){var s=0;for(var i=0;i<n;i++)s+=i*940+941;return s;}
function f940(n){var s=0;for(var i=0;i<n;i++)s+=i*941+942;return s;}
function f941(n){var s=0;for(var i=0;i<n;i++)s+=i*942+943;return s;}
function f942(n){var s=0;for(var i=0;i<n;i++)s+=i*943+944;return s;}
function f943(n){var s=0;for(var i=0;i<n;i++)s+=i*944+945;return s;}
function f944(n){var s=0;for(var i=0;i<n;i++)s+=i*945+946;return s;}
function f945(n){var s=0;for(var i=0;i<n;i++)s+=i*946+947;return s;}
function f946(n){var s=0;for(var i=0;i<n;i++)s+=i*947+948;return s;}
function f947(n){var s=0;for(var i=0;i<n;i++)s+=i*948+949;return s;}
function f948(n){var s=0;for(var i=0;i<n;i++)s+=i*949+950;return s;}
function f949(n){var s=0;for(var i=0;i<n;i++)s+=i*950+951;return s;}
function f950(n){var s=0;for(var i=0;i<n;i++)s+=i*951+952;return s;}
function f951(n){var s=0;for(var i=0;i<n;i++)s+=i*952+953;return s;}
function f952(n){var s=0;for(var i=0;i<n;i++)s+=i*953+954;return s;}
function f953(n){var s=0;for(var i=0;i<n;i++)s+=i*954+955;return s;}
function f954(n){var s=0;for(var i=0;i<n;i++)s+=i*955+956;return s;}
function f955(n){var s=0;for(var i=0;i<n;i++)s+=i*956+957;return s;}
function f956(n){var s=0;for(var i=0;i<n;i++)s+=i*957+958;return s;}
function f957(n){var s=0;for(var i=0;i<n;i++)s+=i*958+959;return s;}
function f958(n){var s=0;for(var i=0;i<n;i++)s+=i*959+960;return s;}
function f959(n){var s=0;for(var i=0;i<n;i++)s+=i*960+961;return s;}
function f960(n){var s=0;for(var i=0;i<n;i++)s+=i*961+962;return s;}
function f961(n){var s=0;for(var i=0;i<n;i++)s+=i*962+963;return s;}
function f962(n){var s=0;for(var i=0;i<n;i++)s+=i*963+964;return s;}
function f963(n){var s=0;for(var i=0;i<n;i++)s+=i*964+965;return s;}
function f964(n){var s=0;for(var i=0;i<n;i++)s+=i*965+966;return s;}
function f965(n){var s=0;for(var i=0;i<n;i++)s+=i*966+967;return s;}
function f966(n){var s=0;for(var i=0;i<n;i++)s+=i*967+968;return s;}
function f967(n){var s=0;for(var i=0;i<n;i++)s+=i*968+969;return s;}
function f968(n){var s=0;for(var i=0;i<n;i++)s+=i*969+970;return s;}
function f969(n){var s=0;for(var i=0;i<n;i++)s+=i*970+971;return s;}
function f970(n){var s=0;for(var i=0;i<n;i++)s+=i*971+972;return s;}
function f971(n){var s=0;for(var i=0;i<n;i++)s+=i*972+973;return s;}
function f972(n){var s=0;for(var i=0;i<n;i++)s+=i*973+974;return s;}
function f973(n){var s=0;for(var i=0;i<n;i++)s+=i*974+975;return s;}
function f974(n){var s=0;for(var i=0;i<n;i++)s+=i*975+976;return s;}
function f975(n){var s=0;for(var i=0;i<n;i++)s+=i*976+977;return s;}
function f976(n){var s=0;for(var i=0;i<n;i++)s+=i*977+978;return s;}
function f977(n){var s=0;for(var i=0;i<n;i++)s+=i*978+979;return s;}
function f978(n){var s=0;for(var i=0;i<n;i++)s+=i*979+980;return s;}
function f979(n){var s=0;for(var i=0;i<n;i++)s+=i*980+981;return s;}
function f980(n){var s=0;for(var i=0;i<n;i++)s+=i*981+982;return s;}
function f981(n){var s=0;for(var i=0;i<n;i++)s+=i*982+983;return s;}
function f982(n){var s=0;for(var i=0;i<n;i++)s+=i*983+984;return s;}
function f983(n){var s=0;for(var i=0;i<n;i++)s+=i*984+985;return s;}
function f984(n){var s=0;for(var i=0;i<n;i++)s+=i*985+986;return s;}
function f985(n){var s=0;for(var i=0;i<n;i++)s+=i*986+987;return s;}
function f986(n){var s=0;for(var i=0;i<n;i++)s+=i*987+988;return s;}
function f987(n){var s=0;for(var i=0;i<n;i++)s+=i*988+989;return s;}
function f988(n){var s=0;for(var i=0;i<n;i++)s+=i*989+990;return s;}
function f989(n){var s=0;for(var i=0;i<n;i++)s+=i*990+991;return s;}
function f990(n){var s=0;for(var i=0;i<n;i++)s+=i*991+992;return s;}
function f991(n){var s=0;for(var i=0;i<n;i++)s+=i*992+993;return s;}
function f992(n){var s=0;for(var i=0;i<n;i++)s+=i*993+994;return s;}
function f993(n){var s=0;for(var i=0;i<n;i++)s+=i*994+995;return s;}
function f994(n){var s=0;for(var i=0;i<n;i++)s+=i*995+996;return s;}
function f995(n){var s=0;for(var i=0;i<n;i++)s+=i*996+997;return s;}
function f996(n){var s=0;for(var i=0;i<n;i++)s+=i*997+998;return s;}
function f997(n){var s=0;for(var i=0;i<n;i++)s+=i*998+999;return s;}
function f998(n){var s=0;for(var i=0;i<n;i++)s+=i*999+1000;return s;}
function f999(n){var s=0;for(var i=0;i<n;i++)s+=i*1000+1001;return s;}

var start = Date.now();
var s = 0;
s += f0(10000);
s += f1(10000);
s += f2(10000);
s += f3(10000);
s += f4(10000);
s += f5(10000);
s += f6(10000);
s += f7(10000);
s += f8(10000);
s += f9(10000);
s += f10(10000);
s += f11(10000);
s += f12(10000);
s += f13(10000);
s += f14(10000);
s += f15(10000);
s += f16(10000);
s += f17(10000);
s += f18(10000);
s += f19(10000);
s += f20(10000);
s += f21(10000);
s += f22(10000);
s += f23(10000);
s += f24(10000);
s += f25(10000);
s += f26(10000);
s += f27(10000);
s += f28(10000);
s += f29(10000);
s += f30(10000);
s += f31(10000);
s += f32(10000);
s += f33(10000);
s += f34(10000);
s += f35(10000);
s += f36(10000);
s += f37(10000);
s += f38(10000);
s += f39(10000);
s += f40(10000);
s += f41(10000);
s += f42(10000);
s += f43(10000);
s += f44(10000);
s += f45(10000);
s += f46(10000);
s += f47(10000);
s += f48(10000);
s += f49(10000);
s += f50(10000);
s += f51(10000);
s += f52(10000);
s += f53(10000);
s += f54(10000);
s += f55(10000);
s += f56(10000);
s += f57(10000);
s += f58(10000);
s += f59(10000);
s += f60(10000);
s += f61(10000);
s += f62(10000);
s += f63(10000);
s += f64(10000);
s += f65(10000);
s += f66(10000);
s += f67(10000);
s += f68(10000);
s += f69(10000);
s += f70(10000);
s += f71(10000);
s += f72(10000);
s += f73(10000);
s += f74(10000);
s += f75(10000);
s += f76(10000);
s += f77(10000);
s += f78(10000);
s += f79(10000);
s += f80(10000);
s += f81(10000);
s += f82(10000);
s += f83(10000);
s += f84(10000);
s += f85(10000);
s += f86(10000);
s += f87(10000);
s += f88(10000);
s += f89(10000);
s += f90(10000);
s += f91(10000);
s += f92(10000);
s += f93(10000);
s += f94(10000);
s += f95(10000);
s += f96(10000);
s += f97(10000);
s += f98(10000);
s += f99(10000);
s += f100(10000);
s += f101(10000);
s += f102(10000);
s += f103(10000);
s += f104(10000);
s += f105(10000);
s += f106(10000);
s += f107(10000);
s += f108(10000);
s += f109(10000);
s += f110(10000);
s += f111(10000);
s += f112(10000);
s += f113(10000);
s += f114(10000);
s += f115(10000);
s += f116(10000);
s += f117(10000);
s += f118(10000);
s += f119(10000);
s += f120(10000);
s += f121(10000);
s += f122(10000);
s += f123(10000);
s += f124(10000);
s += f125(10000);
s += f126(10000);
s += f127(10000);
s += f128(10000);
s += f129(10000);
s += f130(10000);
s += f131(10000);
s += f132(10000);
s += f133(10000);
s += f134(10000);
s += f135(10000);
s += f136(10000);
s += f137(10000);
s += f138(10000);
s += f139(10000);
s += f140(10000);
s += f141(10000);
s += f142(10000);
s += f143(10000);
s += f144(10000);
s += f145(10000);
s += f146(10000);
s += f147(10000);
s += f148(10000);
s += f149(10000);
s += f150(10000);
s += f151(10000);
s += f152(10000);
s += f153(10000);
s += f154(10000);
s += f155(10000);
s += f156(10000);
s += f157(10000);
s += f158(10000);
s += f159(10000);
s += f160(10000);
s += f161(10000);
s += f162(10000);
s += f163(10000);
s += f164(10000);
s += f165(10000);
s += f166(10000);
s += f167(10000);
s += f168(10000);
s += f169(10000);
s += f170(10000);
s += f171(10000);
s += f172(10000);
s += f173(10000);
s += f174(10000);
s += f175(10000);
s += f176(10000);
s += f177(10000);
s += f178(10000);
s += f179(10000);
s += f180(10000);
s += f181(10000);
s += f182(10000);
s += f183(10000);
s += f184(10000);
s += f185(10000);
s += f186(10000);
s += f187(10000);
s += f188(10000);
s += f189(10000);
s += f190(10000);
s += f191(10000);
s += f192(10000);
s += f193(10000);
s += f194(10000);
s += f195(10000);
s += f196(10000);
s += f197(10000);
s += f198(10000);
s += f199(10000);
s += f200(10000);
s += f201(10000);
s += f202(10000);
s += f203(10000);
s += f204(10000);
s += f205(10000);
s += f206(10000);
s += f207(10000);
s += f208(10000);
s += f209(10000);
s += f210(10000);
s += f211(10000);
s += f212(10000);
s += f213(10000);
s += f214(10000);
s += f215(10000);
s += f216(10000);
s += f217(10000);
s += f218(10000);
s += f219(10000);
s += f220(10000);
s += f221(10000);
s += f222(10000);
s += f223(10000);
s += f224(10000);
s += f225(10000);
s += f226(10000);
s += f227(10000);
s += f228(10000);
s += f229(10000);
s += f230(10000);
s += f231(10000);
s += f232(10000);
s += f233(10000);
s += f234(10000);
s += f235(10000);
s += f236(10000);
s += f237(10000);
s += f238(10000);
s += f239(10000);
s += f240(10000);
s += f241(10000);
s += f242(10000);
s += f243(10000);
s += f244(10000);
s += f245(10000);
s += f246(10000);
s += f247(10000);
s += f248(10000);
s += f249(10000);
s += f250(10000);
s += f251(10000);
s += f252(10000);
s += f253(10000);
s += f254(10000);
s += f255(10000);
s += f256(10000);
s += f257(10000);
s += f258(10000);
s += f259(10000);
s += f260(10000);
s += f261(10000);
s += f262(10000);
s += f263(10000);
s += f264(10000);
s += f265(10000);
s += f266(10000);
s += f267(10000);
s += f268(10000);
s += f269(10000);
s += f270(10000);
s += f271(10000);
s += f272(10000);
s += f273(10000);
s += f274(10000);
s += f275(10000);
s += f276(10000);
s += f277(10000);
s += f278(10000);
s += f279(10000);
s += f280(10000);
s += f281(10000);
s += f282(10000);
s += f283(10000);
s += f284(10000);
s += f285(10000);
s += f286(10000);
s += f287(10000);
s += f288(10000);
s += f289(10000);
s += f290(10000);
s += f291(10000);
s += f292(10000);
s += f293(10000);
s += f294(10000);
s += f295(10000);
s += f296(10000);
s += f297(10000);
s += f298(10000);
s += f299(10000);
s += f300(10000);
s += f301(10000);
s += f302(10000);
s += f303(10000);
s += f304(10000);
s += f305(10000);
s += f306(10000);
s += f307(10000);
s += f308(10000);
s += f309(10000);
s += f310(10000);
s += f311(10000);
s += f312(10000);
s += f313(10000);
s += f314(10000);
s += f315(10000);
s += f316(10000);
s += f317(10000);
s += f318(10000);
s += f319(10000);
s += f320(10000);
s += f321(10000);
s += f322(10000);
s += f323(10000);
s += f324(10000);
s += f325(10000);
s += f326(10000);
s += f327(10000);
s += f328(10000);
s += f329(10000);
s += f330(10000);
s += f331(10000);
s += f332(10000);
s += f333(10000);
s += f334(10000);
s += f335(10000);
s += f336(10000);
s += f337(10000);
s += f338(10000);
s += f339(10000);
s += f340(10000);
s += f341(10000);
s += f342(10000);
s += f343(10000);
s += f344(10000);
s += f345(10000);
s += f346(10000);
s += f347(10000);
s += f348(10000);
s += f349(10000);
s += f350(10000);
s += f351(10000);
s += f352(10000);
s += f353(10000);
s += f354(10000);
s += f355(10000);
s += f356(10000);
s += f357(10000);
s += f358(10000);
s += f359(10000);
s += f360(10000);
s += f361(10000);
s += f362(10000);
s += f363(10000);
s += f364(10000);
s += f365(10000);
s += f366(10000);
s += f367(10000);
s += f368(10000);
s += f369(10000);
s += f370(10000);
s += f371(10000);
s += f372(10000);
s += f373(10000);
s += f374(10000);
s += f375(10000);
s += f376(10000);
s += f377(10000);
s += f378(10000);
s += f379(10000);
s += f380(10000);
s += f381(10000);
s += f382(10000);
s += f383(10000);
s += f384(10000);
s += f385(10000);
s += f386(10000);
s += f387(10000);
s += f388(10000);
s += f389(10000);
s += f390(10000);
s += f391(10000);
s += f392(10000);
s += f393(10000);
s += f394(10000);
s += f395(10000);
s += f396(10000);
s += f397(10000);
s += f398(10000);
s += f399(10000);
s += f400(10000);
s += f401(10000);
s += f402(10000);
s += f403(10000);
s += f404(10000);
s += f405(10000);
s += f406(10000);
s += f407(10000);
s += f408(10000);
s += f409(10000);
s += f410(10000);
s += f411(10000);
s += f412(10000);
s += f413(10000);
s += f414(10000);
s += f415(10000);
s += f416(10000);
s += f417(10000);
s += f418(10000);
s += f419(10000);
s += f420(10000);
s += f421(10000);
s += f422(10000);
s += f423(10000);
s += f424(10000);
s += f425(10000);
s += f426(10000);
s += f427(10000);
s += f428(10000);
s += f429(10000);
s += f430(10000);
s += f431(10000);
s += f432(10000);
s += f433(10000);
s += f434(10000);
s += f435(10000);
s += f436(10000);
s += f437(10000);
s += f438(10000);
s += f439(10000);
s += f440(10000);
s += f441(10000);
s += f442(10000);
s += f443(10000);
s += f444(10000);
s += f445(10000);
s += f446(10000);
s += f447(10000);
s += f448(10000);
s += f449(10000);
s += f450(10000);
s += f451(10000);
s += f452(10000);
s += f453(10000);
s += f454(10000);
s += f455(10000);
s += f456(10000);
s += f457(10000);
s += f458(10000);
s += f459(10000);
s += f460(10000);
s += f461(10000);
s += f462(10000);
s += f463(10000);
s += f464(10000);
s += f465(10000);
s += f466(10000);
s += f467(10000);
s += f468(10000);
s += f469(10000);
s += f470(10000);
s += f471(10000);
s += f472(10000);
s += f473(10000);
s += f474(10000);
s += f475(10000);
s += f476(10000);
s += f477(10000);
s += f478(10000);
s += f479(10000);
s += f480(10000);
s += f481(10000);
s += f482(10000);
s += f483(10000);
s += f484(10000);
s += f485(10000);
s += f486(10000);
s += f487(10000);
s += f488(10000);
s += f489(10000);
s += f490(10000);
s += f491(10000);
s += f492(10000);
s += f493(10000);
s += f494(10000);
s += f495(10000);
s += f496(10000);
s += f497(10000);
s += f498(10000);
s += f499(10000);
s += f500(10000);
s += f501(10000);
s += f502(10000);
s += f503(10000);
s += f504(10000);
s += f505(10000);
s += f506(10000);
s += f507(10000);
s += f508(10000);
s += f509(10000);
s += f510(10000);
s += f511(10000);
s += f512(10000);
s += f513(10000);
s += f514(10000);
s += f515(10000);
s += f516(10000);
s += f517(10000);
s += f518(10000);
s += f519(10000);
s += f520(10000);
s += f521(10000);
s += f522(10000);
s += f523(10000);
s += f524(10000);
s += f525(10000);
s += f526(10000);
s += f527(10000);
s += f528(10000);
s += f529(10000);
s += f530(10000);
s += f531(10000);
s += f532(10000);
s += f533(10000);
s += f534(10000);
s += f535(10000);
s += f536(10000);
s += f537(10000);
s += f538(10000);
s += f539(10000);
s += f540(10000);
s += f541(10000);
s += f542(10000);
s += f543(10000);
s += f544(10000);
s += f545(10000);
s += f546(10000);
s += f547(10000);
s += f548(10000);
s += f549(10000);
s += f550(10000);
s += f551(10000);
s += f552(10000);
s += f553(10000);
s += f554(10000);
s += f555(10000);
s += f556(10000);
s += f557(10000);
s += f558(10000);
s += f559(10000);
s += f560(10000);
s += f561(10000);
s += f562(10000);
s += f563(10000);
s += f564(10000);
s += f565(10000);
s += f566(10000);
s += f567(10000);
s += f568(10000);
s += f569(10000);
s += f570(10000);
s += f571(10000);
s += f572(10000);
s += f573(10000);
s += f574(10000);
s += f575(10000);
s += f576(10000);
s += f577(10000);
s += f578(10000);
s += f579(10000);
s += f580(10000);
s += f581(10000);
s += f582(10000);
s += f583(10000);
s += f584(10000);
s += f585(10000);
s += f586(10000);
s += f587(10000);
s += f588(10000);
s += f589(10000);
s += f590(10000);
s += f591(10000);
s += f592(10000);
s += f593(10000);
s += f594(10000);
s += f595(10000);
s += f596(10000);
s += f597(10000);
s += f598(10000);
s += f599(10000);
s += f600(10000);
s += f601(10000);
s += f602(10000);
s += f603(10000);
s += f604(10000);
s += f605(10000);
s += f606(10000);
s += f607(10000);
s += f608(10000);
s += f609(10000);
s += f610(10000);
s += f611(10000);
s += f612(10000);
s += f613(10000);
s += f614(10000);
s += f615(10000);
s += f616(10000);
s += f617(10000);
s += f618(10000);
s += f619(10000);
s += f620(10000);
s += f621(10000);
s += f622(10000);
s += f623(10000);
s += f624(10000);
s += f625(10000);
s += f626(10000);
s += f627(10000);
s += f628(10000);
s += f629(10000);
s += f630(10000);
s += f631(10000);
s += f632(10000);
s += f633(10000);
s += f634(10000);
s += f635(10000);
s += f636(10000);
s += f637(10000);
s += f638(10000);
s += f639(10000);
s += f640(10000);
s += f641(10000);
s += f642(10000);
s += f643(10000);
s += f644(10000);
s += f645(10000);
s += f646(10000);
s += f647(10000);
s += f648(10000);
s += f649(10000);
s += f650(10000);
s += f651(10000);
s += f652(10000);
s += f653(10000);
s += f654(10000);
s += f655(10000);
s += f656(10000);
s += f657(10000);
s += f658(10000);
s += f659(10000);
s += f660(10000);
s += f661(10000);
s += f662(10000);
s += f663(10000);
s += f664(10000);
s += f665(10000);
s += f666(10000);
s += f667(10000);
s += f668(10000);
s += f669(10000);
s += f670(10000);
s += f671(10000);
s += f672(10000);
s += f673(10000);
s += f674(10000);
s += f675(10000);
s += f676(10000);
s += f677(10000);
s += f678(10000);
s += f679(10000);
s += f680(10000);
s += f681(10000);
s += f682(10000);
s += f683(10000);
s += f684(10000);
s += f685(10000);
s += f686(10000);
s += f687(10000);
s += f688(10000);
s += f689(10000);
s += f690(10000);
s += f691(10000);
s += f692(10000);
s += f693(10000);
s += f694(10000);
s += f695(10000);
s += f696(10000);
s += f697(10000);
s += f698(10000);
s += f699(10000);
s += f700(10000);
s += f701(10000);
s += f702(10000);
s += f703(10000);
s += f704(10000);
s += f705(10000);
s += f706(10000);
s += f707(10000);
s += f708(10000);
s += f709(10000);
s += f710(10000);
s += f711(10000);
s += f712(10000);
s += f713(10000);
s += f714(10000);
s += f715(10000);
s += f716(10000);
s += f717(10000);
s += f718(10000);
s += f719(10000);
s += f720(10000);
s += f721(10000);
s += f722(10000);
s += f723(10000);
s += f724(10000);
s += f725(10000);
s += f726(10000);
s += f727(10000);
s += f728(10000);
s += f729(10000);
s += f730(10000);
s += f731(10000);
s += f732(10000);
s += f733(10000);
s += f734(10000);
s += f735(10000);
s += f736(10000);
s += f737(10000);
s += f738(10000);
s += f739(10000);
s += f740(10000);
s += f741(10000);
s += f742(10000);
s += f743(10000);
s += f744(10000);
s += f745(10000);
s += f746(10000);
s += f747(10000);
s += f748(10000);
s += f749(10000);
s += f750(10000);
s += f751(10000);
s += f752(10000);
s += f753(10000);
s += f754(10000);
s += f755(10000);
s += f756(10000);
s += f757(10000);
s += f758(10000);
s += f759(10000);
s += f760(10000);
s += f761(10000);
s += f762(10000);
s += f763(10000);
s += f764(10000);
s += f765(10000);
s += f766(10000);
s += f767(10000);
s += f768(10000);
s += f769(10000);
s += f770(10000);
s += f771(10000);
s += f772(10000);
s += f773(10000);
s += f774(10000);
s += f775(10000);
s += f776(10000);
s += f777(10000);
s += f778(10000);
s += f779(10000);
s += f780(10000);
s += f781(10000);
s += f782(10000);
s += f783(10000);
s += f784(10000);
s += f785(10000);
s += f786(10000);
s += f787(10000);
s += f788(10000);
s += f789(10000);
s += f790(10000);
s += f791(10000);
s += f792(10000);
s += f793(10000);
s += f794(10000);
s += f795(10000);
s += f796(10000);
s += f797(10000);
s += f798(10000);
s += f799(10000);
s += f800(10000);
s += f801(10000);
s += f802(10000);
s += f803(10000);
s += f804(10000);
s += f805(10000);
s += f806(10000);
s += f807(10000);
s += f808(10000);
s += f809(10000);
s += f810(10000);
s += f811(10000);
s += f812(10000);
s += f813(10000);
s += f814(10000);
s += f815(10000);
s += f816(10000);
s += f817(10000);
s += f818(10000);
s += f819(10000);
s += f820(10000);
s += f821(10000);
s += f822(10000);
s += f823(10000);
s += f824(10000);
s += f825(10000);
s += f826(10000);
s += f827(10000);
s += f828(10000);
s += f829(10000);
s += f830(10000);
s += f831(10000);
s += f832(10000);
s += f833(10000);
s += f834(10000);
s += f835(10000);
s += f836(10000);
s += f837(10000);
s += f838(10000);
s += f839(10000);
s += f840(10000);
s += f841(10000);
s += f842(10000);
s += f843(10000);
s += f844(10000);
s += f845(10000);
s += f846(10000);
s += f847(10000);
s += f848(10000);
s += f849(10000);
s += f850(10000);
s += f851(10000);
s += f852(10000);
s += f853(10000);
s += f854(10000);
s += f855(10000);
s += f856(10000);
s += f857(10000);
s += f858(10000);
s += f859(10000);
s += f860(10000);
s += f861(10000);
s += f862(10000);
s += f863(10000);
s += f864(10000);
s += f865(10000);
s += f866(10000);
s += f867(10000);
s += f868(10000);
s += f869(10000);
s += f870(10000);
s += f871(10000);
s += f872(10000);
s += f873(10000);
s += f874(10000);
s += f875(10000);
s += f876(10000);
s += f877(10000);
s += f878(10000);
s += f879(10000);
s += f880(10000);
s += f881(10000);
s += f882(10000);
s += f883(10000);
s += f884(10000);
s += f885(10000);
s += f886(10000);
s += f887(10000);
s += f888(10000);
s += f889(10000);
s += f890(10000);
s += f891(10000);
s += f892(10000);
s += f893(10000);
s += f894(10000);
s += f895(10000);
s += f896(10000);
s += f897(10000);
s += f898(10000);
s += f899(10000);
s += f900(10000);
s += f901(10000);
s += f902(10000);
s += f903(10000);
s += f904(10000);
s += f905(10000);
s += f906(10000);
s += f907(10000);
s += f908(10000);
s += f909(10000);
s += f910(10000);
s += f911(10000);
s += f912(10000);
s += f913(10000);
s += f914(10000);
s += f915(10000);
s += f916(10000);
s += f917(10000);
s += f918(10000);
s += f919(10000);
s += f920(10000);
s += f921(10000);
s += f922(10000);
s += f923(10000);
s += f924(10000);
s += f925(10000);
s += f926(10000);
s += f927(10000);
s += f928(10000);
s += f929(10000);
s += f930(10000);
s += f931(10000);
s += f932(10000);
s += f933(10000);
s += f934(10000);
s += f935(10000);
s += f936(10000);
s += f937(10000);
s += f938(10000);
s += f939(10000);
s += f940(10000);
s += f941(10000);
s += f942(10000);
s += f943(10000);
s += f944(10000);
s += f945(10000);
s += f946(10000);
s += f947(10000);
s += f948(10000);
s += f949(10000);
s += f950(10000);
s += f951(10000);
s += f952(10000);
s += f953(10000);
s += f954(10000);
s += f955(10000);
s += f956(10000);
s += f957(10000);
s += f958(10000);
s += f959(10000);
s += f960(10000);
s += f961(10000);
s += f962(10000);
s += f963(10000);
s += f964(10000);
s += f965(10000);
s += f966(10000);
s += f967(10000);
s += f968(10000);
s += f969(10000);
s += f970(10000);
s += f971(10000);
s += f972(10000);
s += f973(10000);
s += f974(10000);
s += f975(10000);
s += f976(10000);
s += f977(10000);
s += f978(10000);
s += f979(10000);
s += f980(10000);
s += f981(10000);
s += f982(10000);
s += f983(10000);
s += f984(10000);
s += f985(10000);
s += f986(10000);
s += f987(10000);
s += f988(10000);
s += f989(10000);
s += f990(10000);
s += f991(10000);
s += f992(10000);
s += f993(10000);
s += f994(10000);
s += f995(10000);
s += f996(10000);
s += f997(10000);
s += f998(10000);
s += f999(10000);
var elapsed = (Date.now() - start) / 1000;
console.log("s = " + s);
console.log("elapsed: " + elapsed + "s");
