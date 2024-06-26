// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed into the Public Domain, for any use,
// without warranty, 2012 by Iztok Jeras.
// SPDX-License-Identifier: CC0-1.0

module t (/*AUTOARG*/
   // Inputs
   clk
   );

   input clk;

   // parameters for array sizes
   localparam WA = 8;  // address dimension size
   localparam WB = 8;  // bit     dimension size

   localparam NO = 10;  // number of access events

   // 2D packed arrays
   logic        [WA-1:0] [WB-1:0] array_dsc;  // descending range array
   /* verilator lint_off ASCRANGE */
   logic        [0:WA-1] [0:WB-1] array_asc;  // ascending range array
   /* verilator lint_on ASCRANGE */

   integer cnt = 0;

   // msg926
   logic [3:0][31:0] packedArray;
   initial packedArray = '0;

   // event counter
   always @ (posedge clk) begin
      cnt <= cnt + 1;
   end

   // finish report
   always @ (posedge clk)
   if ((cnt[30:2]==NO) && (cnt[1:0]==2'd0)) begin
      $write("*-* All Finished *-*\n");
      $finish;
   end

   // descending range
   always @ (posedge clk)
   if (cnt[1:0]==2'd0) begin
      // initialize to defaaults (all bits to 0)
      if      (cnt[30:2]==0)  array_dsc <= '0;
      else if (cnt[30:2]==1)  array_dsc <= '0;
      else if (cnt[30:2]==2)  array_dsc <= '0;
      else if (cnt[30:2]==3)  array_dsc <= '0;
      else if (cnt[30:2]==4)  array_dsc <= '0;
      else if (cnt[30:2]==5)  array_dsc <= '0;
      else if (cnt[30:2]==6)  array_dsc <= '0;
      else if (cnt[30:2]==7)  array_dsc <= '0;
      else if (cnt[30:2]==8)  array_dsc <= '0;
      else if (cnt[30:2]==9)  array_dsc <= '0;
   end else if (cnt[1:0]==2'd1) begin
      // write value to array
      if      (cnt[30:2]==0)  begin end
      else if (cnt[30:2]==1)  array_dsc                            <= {WA  *WB  +0{1'b1}};
      else if (cnt[30:2]==2)  array_dsc [WA/2-1:0   ]              <= {WA/2*WB  +0{1'b1}};
      else if (cnt[30:2]==3)  array_dsc [WA  -1:WA/2]              <= {WA/2*WB  +0{1'b1}};
      else if (cnt[30:2]==4)  array_dsc [       0   ]              <= {1   *WB  +0{1'b1}};
      else if (cnt[30:2]==5)  array_dsc [WA  -1     ]              <= {1   *WB  +0{1'b1}};
      else if (cnt[30:2]==6)  array_dsc [       0   ][WB/2-1:0   ] <= {1   *WB/2+0{1'b1}};
      else if (cnt[30:2]==7)  array_dsc [WA  -1     ][WB  -1:WB/2] <= {1   *WB/2+0{1'b1}};
      else if (cnt[30:2]==8)  array_dsc [       0   ][       0   ] <= {1   *1   +0{1'b1}};
      else if (cnt[30:2]==9)  array_dsc [WA  -1     ][WB  -1     ] <= {1   *1   +0{1'b1}};
   end else if (cnt[1:0]==2'd2) begin
      // check array value
      if      (cnt[30:2]==0)  begin if (array_dsc !== 64'b0000000000000000000000000000000000000000000000000000000000000000) begin $display("%b", array_dsc); $stop(); end end
      else if (cnt[30:2]==1)  begin if (array_dsc !== 64'b1111111111111111111111111111111111111111111111111111111111111111) begin $display("%b", array_dsc); $stop(); end end
      else if (cnt[30:2]==2)  begin if (array_dsc !== 64'b0000000000000000000000000000000011111111111111111111111111111111) begin $display("%b", array_dsc); $stop(); end end
      else if (cnt[30:2]==3)  begin if (array_dsc !== 64'b1111111111111111111111111111111100000000000000000000000000000000) begin $display("%b", array_dsc); $stop(); end end
      else if (cnt[30:2]==4)  begin if (array_dsc !== 64'b0000000000000000000000000000000000000000000000000000000011111111) begin $display("%b", array_dsc); $stop(); end end
      else if (cnt[30:2]==5)  begin if (array_dsc !== 64'b1111111100000000000000000000000000000000000000000000000000000000) begin $display("%b", array_dsc); $stop(); end end
      else if (cnt[30:2]==6)  begin if (array_dsc !== 64'b0000000000000000000000000000000000000000000000000000000000001111) begin $display("%b", array_dsc); $stop(); end end
      else if (cnt[30:2]==7)  begin if (array_dsc !== 64'b1111000000000000000000000000000000000000000000000000000000000000) begin $display("%b", array_dsc); $stop(); end end
      else if (cnt[30:2]==8)  begin if (array_dsc !== 64'b0000000000000000000000000000000000000000000000000000000000000001) begin $display("%b", array_dsc); $stop(); end end
      else if (cnt[30:2]==9)  begin if (array_dsc !== 64'b1000000000000000000000000000000000000000000000000000000000000000) begin $display("%b", array_dsc); $stop(); end end
   end else if (cnt[1:0]==2'd3) begin
      // read value from array (not a very good test for now)
      if      (cnt[30:2]==0)  begin if (array_dsc                            !== {WA  *WB    {1'b0}}) $stop(); end
      else if (cnt[30:2]==1)  begin if (array_dsc                            !== {WA  *WB  +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==2)  begin if (array_dsc [WA/2-1:0   ]              !== {WA/2*WB  +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==3)  begin if (array_dsc [WA  -1:WA/2]              !== {WA/2*WB  +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==4)  begin if (array_dsc [       0   ]              !== {1   *WB  +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==5)  begin if (array_dsc [WA  -1     ]              !== {1   *WB  +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==6)  begin if (array_dsc [       0   ][WB/2-1:0   ] !== {1   *WB/2+0{1'b1}}) $stop(); end
      else if (cnt[30:2]==7)  begin if (array_dsc [WA  -1     ][WB  -1:WB/2] !== {1   *WB/2+0{1'b1}}) $stop(); end
      else if (cnt[30:2]==8)  begin if (array_dsc [       0   ][       0   ] !== {1   *1   +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==9)  begin if (array_dsc [WA  -1     ][WB  -1     ] !== {1   *1   +0{1'b1}}) $stop(); end
   end

   // ascending range
   always @ (posedge clk)
   if (cnt[1:0]==2'd0) begin
      // initialize to defaaults (all bits to 0)
      if      (cnt[30:2]==0)  array_asc <= '0;
      else if (cnt[30:2]==1)  array_asc <= '0;
      else if (cnt[30:2]==2)  array_asc <= '0;
      else if (cnt[30:2]==3)  array_asc <= '0;
      else if (cnt[30:2]==4)  array_asc <= '0;
      else if (cnt[30:2]==5)  array_asc <= '0;
      else if (cnt[30:2]==6)  array_asc <= '0;
      else if (cnt[30:2]==7)  array_asc <= '0;
      else if (cnt[30:2]==8)  array_asc <= '0;
      else if (cnt[30:2]==9)  array_asc <= '0;
   end else if (cnt[1:0]==2'd1) begin
      // write value to array
      if      (cnt[30:2]==0)  begin end
      else if (cnt[30:2]==1)  array_asc                            <= {WA  *WB  +0{1'b1}};
      else if (cnt[30:2]==2)  array_asc [0   :WA/2-1]              <= {WA/2*WB  +0{1'b1}};
      else if (cnt[30:2]==3)  array_asc [WA/2:WA  -1]              <= {WA/2*WB  +0{1'b1}};
      else if (cnt[30:2]==4)  array_asc [0          ]              <= {1   *WB  +0{1'b1}};
      else if (cnt[30:2]==5)  array_asc [     WA  -1]              <= {1   *WB  +0{1'b1}};
      else if (cnt[30:2]==6)  array_asc [0          ][0   :WB/2-1] <= {1   *WB/2+0{1'b1}};
      else if (cnt[30:2]==7)  array_asc [     WA  -1][WB/2:WB  -1] <= {1   *WB/2+0{1'b1}};
      else if (cnt[30:2]==8)  array_asc [0          ][0          ] <= {1   *1   +0{1'b1}};
      else if (cnt[30:2]==9)  array_asc [     WA  -1][     WB  -1] <= {1   *1   +0{1'b1}};
   end else if (cnt[1:0]==2'd2) begin
      // check array value
      if      (cnt[30:2]==0)  begin if (array_asc !== 64'b0000000000000000000000000000000000000000000000000000000000000000) begin $display("%b", array_asc); $stop(); end end
      else if (cnt[30:2]==1)  begin if (array_asc !== 64'b1111111111111111111111111111111111111111111111111111111111111111) begin $display("%b", array_asc); $stop(); end end
      else if (cnt[30:2]==2)  begin if (array_asc !== 64'b1111111111111111111111111111111100000000000000000000000000000000) begin $display("%b", array_asc); $stop(); end end
      else if (cnt[30:2]==3)  begin if (array_asc !== 64'b0000000000000000000000000000000011111111111111111111111111111111) begin $display("%b", array_asc); $stop(); end end
      else if (cnt[30:2]==4)  begin if (array_asc !== 64'b1111111100000000000000000000000000000000000000000000000000000000) begin $display("%b", array_asc); $stop(); end end
      else if (cnt[30:2]==5)  begin if (array_asc !== 64'b0000000000000000000000000000000000000000000000000000000011111111) begin $display("%b", array_asc); $stop(); end end
      else if (cnt[30:2]==6)  begin if (array_asc !== 64'b1111000000000000000000000000000000000000000000000000000000000000) begin $display("%b", array_asc); $stop(); end end
      else if (cnt[30:2]==7)  begin if (array_asc !== 64'b0000000000000000000000000000000000000000000000000000000000001111) begin $display("%b", array_asc); $stop(); end end
      else if (cnt[30:2]==8)  begin if (array_asc !== 64'b1000000000000000000000000000000000000000000000000000000000000000) begin $display("%b", array_asc); $stop(); end end
      else if (cnt[30:2]==9)  begin if (array_asc !== 64'b0000000000000000000000000000000000000000000000000000000000000001) begin $display("%b", array_asc); $stop(); end end
   end else if (cnt[1:0]==2'd3) begin
      // read value from array (not a very good test for now)
      if      (cnt[30:2]==0)  begin if (array_asc                            !== {WA  *WB    {1'b0}}) $stop(); end
      else if (cnt[30:2]==1)  begin if (array_asc                            !== {WA  *WB  +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==2)  begin if (array_asc [0   :WA/2-1]              !== {WA/2*WB  +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==3)  begin if (array_asc [WA/2:WA  -1]              !== {WA/2*WB  +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==4)  begin if (array_asc [0          ]              !== {1   *WB  +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==5)  begin if (array_asc [     WA  -1]              !== {1   *WB  +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==6)  begin if (array_asc [0          ][0   :WB/2-1] !== {1   *WB/2+0{1'b1}}) $stop(); end
      else if (cnt[30:2]==7)  begin if (array_asc [     WA  -1][WB/2:WB  -1] !== {1   *WB/2+0{1'b1}}) $stop(); end
      else if (cnt[30:2]==8)  begin if (array_asc [0          ][0          ] !== {1   *1   +0{1'b1}}) $stop(); end
      else if (cnt[30:2]==9)  begin if (array_asc [     WA  -1][     WB  -1] !== {1   *1   +0{1'b1}}) $stop(); end
   end

endmodule
