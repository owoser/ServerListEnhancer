package cn.owoser.sle;

import java.util.Arrays;
import java.awt.image.BufferedImage;
import javax.imageio.ImageIO;
import java.io.File;

public final class HomeEntryInteractionTest {
    private static int checks;
    private static void check(boolean value,String label) {
        ++checks;
        if (!value) throw new AssertionError(label);
    }
    private static void block(int[] pixels,int width,int left,int top,int side,int color) {
        for(int y=top;y<top+side;++y) Arrays.fill(pixels,y*width+left,y*width+left+side,color);
    }
    private static HomeOverlay.EntryTap tap(float x,float y) { return new HomeOverlay.EntryTap(x,y,x,y); }
    private static void geometry(float scale,int originX,int originY) {
        int width=1200,height=700,left=700,top=30;
        int w=Math.round(50*scale),h=Math.round(24*scale),side=Math.round(2*scale);
        int[] pixels=new int[width*height];Arrays.fill(pixels,0xffc6c6c6);
        block(pixels,width,left,top,side,HomeOverlay.EntryBounds.TOP_COLOR);
        block(pixels,width,left+w-side,top+h-side,side,HomeOverlay.EntryBounds.BOTTOM_COLOR);
        // Distractors that defeated the old whole-screen min/max blue-channel scan.
        block(pixels,width,0,180,3,HomeOverlay.EntryBounds.TOP_COLOR);
        block(pixels,width,356,185,3,HomeOverlay.EntryBounds.BOTTOM_COLOR);
        HomeOverlay.EntryTap inside=tap(left+w/2f+originX,top+h/2f+originY);
        HomeOverlay.EntryBounds found=HomeOverlay.EntryBounds.find(pixels.clone(),width,height,inside,originX,originY);
        check(found!=null&&found.left==left+originX&&found.top==top+originY
                &&found.right==left+w+originX&&found.bottom==top+h+originY,"actual scaled bounds "+scale);
        check(HomeOverlay.EntryBounds.find(pixels.clone(),width,height,tap(left+w+originX,top+originY),originX,originY)==null,"outside rendered right edge "+scale);
        check(HomeOverlay.EntryBounds.find(pixels.clone(),width,height,
                new HomeOverlay.EntryTap(left-1+originX,top+originY,inside.upX,inside.upY),originX,originY)==null,"down outside cannot activate "+scale);
        if(scale==3f) check(HomeOverlay.EntryBounds.find(pixels.clone(),width,height,tap(960,65),originX,originY)==null,"old 100% hitbox rejected at 75% scale");
    }
    public static void main(String[] args) throws Exception {
        if(args.length==3) {
            BufferedImage image=ImageIO.read(new File(args[0]));
            int width=image.getWidth(),height=image.getHeight();
            HomeOverlay.EntryBounds bounds=HomeOverlay.EntryBounds.find(image.getRGB(0,0,width,height,null,0,width),width,height,
                    tap(Float.parseFloat(args[1]),Float.parseFloat(args[2])),0,0);
            if(bounds==null) throw new AssertionError("no valid entry at supplied screenshot coordinates");
            System.out.println(bounds.left+","+bounds.top+".."+bounds.right+","+bounds.bottom);
            return;
        }
        for(float scale:new float[]{1f,1.25f,1.5f,2.25f,3f,4f,6f}) geometry(scale,23,17);
        HomeOverlay.EntryGesture gesture=new HomeOverlay.EntryGesture();
        check(gesture.event(1,0,10,10,8)==null,"orphan UP");
        gesture.event(0,7,10,10,8);
        check(gesture.event(1,7,12,12,8)!=null,"same pointer tap");
        check(gesture.event(1,7,12,12,8)==null,"tap fires once");
        gesture.event(0,7,10,10,8);gesture.event(2,7,20,10,8);
        check(gesture.event(1,7,10,10,8)==null,"drag out and back cancelled");
        gesture.event(0,7,10,10,8);gesture.event(3,7,10,10,8);
        check(gesture.event(1,7,10,10,8)==null,"ACTION_CANCEL");
        gesture.event(0,7,10,10,8);gesture.event(5,8,10,10,8);
        check(gesture.event(1,7,10,10,8)==null,"second pointer cancels tap");
        gesture.event(0,7,10,10,8);
        check(gesture.event(1,8,10,10,8)==null,"other pointer UP ignored");
        check(gesture.event(1,7,10,10,8)!=null,"original pointer release still valid");
        check(HomeOverlay.EntryBounds.find(new int[8],3,3,tap(0,0),0,0)==null,"invalid screenshot dimensions");
        int[] plain=new int[600*300];Arrays.fill(plain,0xffc6c6c6);
        check(HomeOverlay.EntryBounds.find(plain,600,300,tap(500,30),0,0)==null,"marker-free menu cannot open entry");
        block(plain,600,450,10,4,HomeOverlay.EntryBounds.TOP_COLOR);
        check(HomeOverlay.EntryBounds.find(plain,600,300,tap(500,30),0,0)==null,"partial marker cannot open entry");
        System.out.println("Android entry geometry, fractional GUI scales, safezone origin, noise and gesture lifecycle: PASS ("+checks+" checks)");
    }
}
