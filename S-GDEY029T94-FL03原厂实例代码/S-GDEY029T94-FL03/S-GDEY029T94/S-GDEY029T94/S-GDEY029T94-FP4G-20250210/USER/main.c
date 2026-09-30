#include "stm32f10x.h"
#include "delay.h"
#include "sys.h"
//EPD
#include "Display_EPD_W21_spi.h"
#include "Display_EPD_W21.h"
#include "Ap_29demo.h"	
//GUI
#include "GUI_Paint.h"
#include "fonts.h"

unsigned char BlackImage[EPD_ARRAY];//Define canvas space  

//Tips//
/*
1.Flickering is normal when EPD is performing a full screen update to clear ghosting from the previous image so to ensure better clarity and legibility for the new image.
2.There will be no flicker when EPD performs a partial update.
3.Please make sue that EPD enters sleep mode when update is completed and always leave the sleep mode command. Otherwise, this may result in a reduced lifespan of EPD.
4.Please refrain from inserting EPD to the FPC socket or unplugging it when the MCU is being powered to prevent potential damage.)
5.Re-initialization is required for every full screen update.
6.When porting the program, set the BUSY pin to input mode and other pins to output mode.
*/

int	main(void)
{
	  unsigned char i;
		delay_init();	    	     //Delay function initialization
		NVIC_Configuration(); 	//Set NVIC interrupt grouping 2
    EPD_GPIO_Init();       //EPD GPIO  initialization
	while(1)
	{    

#if 1 //Full screen update, fast update, and partial update demostration.

		 /************Full display(2s)*******************/
			EPD_HW_Init(); //Full screen update initialization.
			EPD_WhiteScreen_ALL(gImage_1); //To Display one image using full screen update.
			EPD_DeepSleep(); //Enter the sleep mode and please do not delete it, otherwise it will reduce the lifespan of the screen.
			delay_s(2); //Delay for 2s.							
			/************Fast update mode(1.5s)*******************/
			EPD_HW_Init_Fast(); //Fast update initialization.
			EPD_WhiteScreen_ALL_Fast(gImage_2); //To display the second image using fast update.
			EPD_DeepSleep(); //Enter the sleep mode and please do not delete it, otherwise it will reduce the lifespan of the screen.
			delay_s(2); //Delay for 2s.		
	     /************4 Gray update mode(2s)*******************/
	#if 0	 //To enable this feature, please change 0 to 1
			/************Onder versions do not support this feature*******************/
			EPD_HW_Init_4G(); //Fast update initialization.
			EPD_WhiteScreen_ALL_4G(gImage_4G1); //To display one image using fast update.
			EPD_DeepSleep(); //Enter the sleep mode and please do not delete it, otherwise it will reduce the lifespan of the screen.
			delay_s(2); //Delay for 2s.	
	#endif
	#if 1 //Partial update demostration.
	//Partial update demo support displaying a clock at 5 locations with 00:00.  If you need to perform partial update more than 5 locations, please use the feature of using partial update at the full screen demo.
	//After 5 partial updatees, implement a full screen update to clear the ghosting caused by partial updatees.
	//////////////////////Partial update time demo/////////////////////////////////////
			EPD_HW_Init(); //Electronic paper initialization.	
			EPD_SetRAMValue_BaseMap(gImage_basemap); //Please do not delete the background color function, otherwise it will cause unstable display during partial update.
			for(i=0;i<6;i++)
			EPD_Dis_Part_Time(32,100+32*0,Num[i],         //x-A,y-A,DATA-A
												32,100+32*1,Num[0],         //x-B,y-B,DATA-B
												32,100+32*2,gImage_numdot, //x-C,y-C,DATA-C
												32,100+32*3,Num[0],        //x-D,y-D,DATA-D
												32,100+32*4,Num[1],32,64); //x-E,y-E,DATA-E,Resolution 32*64
    
			EPD_DeepSleep();  //Enter the sleep mode and please do not delete it, otherwise it will reduce the lifespan of the screen.
			delay_s(2);	//Delay for 2s.

	#endif	

  #if 0 //Demo of using partial update to update the full screen, to enable this feature, please change 0 to 1.
	//After 5 partial updatees, implement a full screen update to clear the ghosting caused by partial updatees.
	//////////////////////Partial update time demo/////////////////////////////////////
			EPD_HW_Init(); //E-paper initialization	
			EPD_SetRAMValue_BaseMap(gImage_p1); //Please do not delete the background color function, otherwise it will cause an unstable display during partial update.
			EPD_Dis_PartAll(gImage_p1); //Image 1
			EPD_Dis_PartAll(gImage_p2); //Image 2
			EPD_Dis_PartAll(gImage_p3); //Image 3
			EPD_Dis_PartAll(gImage_p4); //Image 4
			EPD_Dis_PartAll(gImage_p5); //Image 5	
			EPD_DeepSleep();//Enter the sleep mode and please do not delete it, otherwise it will reduce the lifespan of the screen.
			delay_s(2); //Delay for 2s.	
			EPD_HW_Init(); //Full screen update initialization.
			EPD_WhiteScreen_White(); //Clear screen function.
			EPD_DeepSleep(); //Enter the sleep mode and please do not delete it, otherwise it will reduce the lifespan of the screen.
			delay_s(2);	//Delay for 2s.
	#endif
	
	#if 0 //Demonstration of full screen update with 180-degree rotation, to enable this feature, please change 0 to 1.
			/************Full display(2s)*******************/
			EPD_HW_Init_180(); //Full screen update initialization.
			EPD_WhiteScreen_ALL(gImage_1); //To Display one image using full screen update.
			EPD_DeepSleep(); //Enter the sleep mode and please do not delete it, otherwise it will reduce the lifespan of the screen.
			delay_s(2); //Delay for 2s.
	#endif				
	
#endif
	



		
#if 0 //GUI Demo(GUI examples can display points, lines, rectangles, circles, letters, numbers, etc).
   //Data initialization settings.
    Paint_NewImage(BlackImage, EPD_WIDTH, EPD_HEIGHT, 270, WHITE); //Set canvas parameters, GUI image rotation, please change 270 to 0/90/180/270.
    Paint_SelectImage(BlackImage); //Select current settings.
    /**************Drawing demonstration**********************/   
		EPD_HW_Init_GUI(); //GUI initialization.
		Paint_Clear(WHITE); //Clear canvas.
		//Point.   
    Paint_DrawPoint(5, 10, BLACK, DOT_PIXEL_1X1, DOT_STYLE_DFT); //point 1x1.
    Paint_DrawPoint(5, 25, BLACK, DOT_PIXEL_2X2, DOT_STYLE_DFT); //point 2x2.
    Paint_DrawPoint(5, 40, BLACK, DOT_PIXEL_3X3, DOT_STYLE_DFT); //point 3x3.
    Paint_DrawPoint(5, 55, BLACK, DOT_PIXEL_4X4, DOT_STYLE_DFT); //point 4x4.
		//Line.
    Paint_DrawLine(20, 10, 70, 60, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1); //1x1line 1.
    Paint_DrawLine(70, 10, 20, 60, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1); //1x1line 2.
		//Rectangle.
    Paint_DrawRectangle(20, 10, 70, 60, BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_1X1); //Hollow rectangle 1.
    Paint_DrawRectangle(85, 10, 130, 60, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1); //Hollow rectangle 2.
    //Circle.
		Paint_DrawCircle(150, 90, 30, BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_1X1); //Hollow circle.
    Paint_DrawCircle(200, 90, 30, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1); //solid circle.
    EPD_Display(BlackImage); //Display GUI image.
		EPD_DeepSleep();//EPD_DeepSleep,Sleep instruction is necessary, please do not delete!!!
    delay_s(2); //Delay for 2s.		
		
    /***********Letter demo***************************/
		EPD_HW_Init_GUI(); //GUI initialization.
		Paint_Clear(WHITE); //Clear canvas.
    Paint_DrawString_EN(0, 0, "Good Display", &Font8, WHITE, BLACK);  //5*8.
		Paint_DrawString_EN(0, 10, "Good Display", &Font12, WHITE, BLACK); //7*12.
		Paint_DrawString_EN(0, 25, "Good Display", &Font16, WHITE, BLACK); //11*16.
		Paint_DrawString_EN(0, 45, "Good Display", &Font20, WHITE, BLACK); //14*20.
		Paint_DrawString_EN(0, 80, "Good Display", &Font24, WHITE, BLACK); //17*24.
    EPD_Display(BlackImage);//Display GUI image.
		EPD_DeepSleep(); //EPD_DeepSleep,Sleep instruction is necessary, please do not delete!!!
    delay_s(2); //Delay for 2s.
			
		/*************Numbers demo************************/
		EPD_HW_Init_GUI(); //GUI initialization.
		Paint_Clear(WHITE); //Clear canvas.
    Paint_DrawNum(0, 0, 123456789, &Font8, WHITE, BLACK);  //5*8.
		Paint_DrawNum(0, 10, 123456789, &Font12, WHITE, BLACK); //7*12.
		Paint_DrawNum(0, 25, 123456789, &Font16, WHITE, BLACK); //11*16.
		Paint_DrawNum(0, 45, 123456789, &Font20, WHITE, BLACK); //14*20.
		Paint_DrawNum(0, 70, 123456789, &Font24, WHITE, BLACK); //17*24.
    EPD_Display(BlackImage); //Display GUI image.
		EPD_DeepSleep();//EPD_DeepSleep,Sleep instruction is necessary, please do not delete!!!
    delay_s(2); //Delay for 2s.	 			

#endif
		//Full screen update clear the screen.
		EPD_HW_Init(); //Full screen update initialization.
		EPD_WhiteScreen_White(); //Clear screen function.
		EPD_DeepSleep(); //Enter the sleep mode and please do not delete it, otherwise it will reduce the lifespan of the screen.
		delay_s(2);	//Delay for 2s.	
  while(1);	// The program stops here
	}
}	


